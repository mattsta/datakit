#include "datakit.h"

#include "jebuf.h"
#include "multilru.h"
#include "timeUtil.h"

#include <inttypes.h> /* PRIu32 */
#include <string.h>   /* memcmp */

#define ENTRY_SIZE 9
#define USE_PACKED 1

#if USE_PACKED
#define PACK __attribute__((packed))
#else
#define PACK
#endif

typedef struct PACK lruEntry {
#if ENTRY_SIZE == 9
    /* Maximum 4 billion entries per LRU */
    /* 9 byte struct (when __attribute__((packed))) */
    uint32_t prev;
    uint32_t next;
    uint8_t currentLevel : 6;
    uint8_t isPopulated : 1;
    uint8_t isHeadNode : 1;
#elif ENTRY_SIZE == 16
    /* Maximum of 1,000 trillion entries */
    /* _16 byte struct */
    __uint128_t prev : 60;
    __uint128_t next : 60;
    __uint128_t currentLevel : 6;
    __uint128_t isPopulated : 1;
    __uint128_t isHeadNode : 1;
#elif ENTRY_SIZE == 26
    /* Maximum 18 thousand trillion entries */
    /* These make debugging easier because lldb doesn't like
     * printing the value of 128-bit bitfields. */
    size_t prev;
    size_t next;
    size_t currentLevel;
    bool isHeadNode;
    bool isPopulated;
#else
#error "No ENTRY_SIZE defined for LRU!"
#endif
} lruEntry;

struct multilru {
    /* linear malloc array of 'lruEntry' */
    /* TODO: if this grows too big, maybe split into multiple
     *       levels or even a multiarray */
    lruEntry *entries;

    /* Reference points for discovering gaps in
     * ptr assignments. Hopefully we can easily locate
     * unused ptr positions instead of growing the array
     * unnecessarily. */
    uint32_t highestAllocated;
    uint32_t freePosition[256];

    /* Basic usage:
     *  - get the lowest entry
     *  - get each of the level markers */
    multilruPtr lowest;
    multilruPtr *level;

    /* Count of all active entries in the LRU */
    size_t count;
    size_t maxLevels;
};

ssize_t multilruTraverseSize(const multilru *mlru);
DK_STATIC void removeEntryFromCurrentPosition(multilru *mlru,
                                              const lruEntry *entryCurrent);

//        printf("Assinging lowest as %d at %d\n", l, __LINE__);
#define assignLowest(l)                                                        \
    do {                                                                       \
        mlru->lowest = (l);                                                    \
    } while (0)
/* ====================================================================
 * Freedom Manipulation
 * ==================================================================== */
/* Shorthand for indexing into our 'entries' array.
 * Assumes we always call our multilru pointers 'mlru' which is true and
 * saves on redundant keystrokes everywhere. */
#define entry(ptr) (&mlru->entries[ptr])

DK_STATIC bool markFree(multilru *mlru, const multilruPtr ptr) {
    if (ptr >= mlru->highestAllocated) {
        /* This happens normally when we try to grow our free
         * pointers using the counter and the counter increments
         * beyond our total size... */
        return false;
    }

    /* Find empty free slot for re-using removed entry */
    for (size_t i = 0; i < COUNT_ARRAY(mlru->freePosition); i++) {
        /* TODO: could cache free position in free list...
         *       a free list list scalar */
        if (mlru->freePosition[i] == 0) {
            mlru->freePosition[i] = ptr;
            return true;
        }
    }

    /* If we got this far, all the free slots were already used,
     * so 'ptr' will just dangle as free without any way to easily
     * reach it again without iterating... */
    return false;
}

DK_STATIC void markFreeCleanup(multilru *mlru, const multilruPtr ptr) {
    lruEntry *e = entry(ptr);
    assert(e->isPopulated);

    memset(e, 0, sizeof(*e));

    assert(mlru->count > 0);
    mlru->count--;

    markFree(mlru, ptr);
}

DK_STATIC void markFreeStarting(multilru *mlru, multilruPtr ptr) {
    /* Attempt to set up to 'freePositions' new entries as empty... */
    for (size_t i = 0; i < COUNT_ARRAY(mlru->freePosition); i++) {
        if (!markFree(mlru, ptr++)) {
            break;
        }
    }
}

DK_STATIC void markFreeDiscover(multilru *mlru) {
    size_t freePos = 0;
    /* TODO: this is obviously slow. We could optionally
     *       speed it up if we just made freePosition a
     *       multilist, which would be fairly small since
     *       all entries are integers.
     *       Though, for 300,000 multilruPtr values, a
     *       multilist would be almost 8 MB, so not *the* smallest,
     *       but much faster than this linear scan trying to
     *       find free entries. */
    for (size_t i = 1; i < mlru->highestAllocated; i++) {
#if 0
        printf("Checking ptr %d...\n", i);
#endif
        if (entry(i)->isPopulated) {
            continue;
        }

        assert(i > mlru->maxLevels);

#if 0
        printf("Setting free (%zu, %zu)\n", freePos, i);
#endif
        mlru->freePosition[freePos++] = i;

        /* If freePos at highest free slot, we're done now. */
        if (freePos == COUNT_ARRAY(mlru->freePosition)) {
            break;
        }
    }
}

DK_STATIC multilruPtr getAvailable(multilru *mlru) {
    for (size_t i = 0; i < COUNT_ARRAY(mlru->freePosition); i++) {
        const multilruPtr ptrAt = mlru->freePosition[i];
        if (ptrAt) {
            mlru->freePosition[i] = 0;
#if 0
            printf("Got available: (%zu, %zu)\n", i, ptrAt);
#endif
            return ptrAt;
        }
    }

    /* If we get here, ALL of the free list is zero, BUT we know
     * we have free entries elsewhere.  Iterate to find at least one. */
    /* ergo, this shouldn't (really, it shouldn't) be an infinite loop.
     * It'll only infinite loop if our add/remove count math is wrong. */
    markFreeDiscover(mlru);

    for (size_t i = 0; i < COUNT_ARRAY(mlru->freePosition); i++) {
        const multilruPtr ptrAt = mlru->freePosition[i];
        if (ptrAt) {
            mlru->freePosition[i] = 0;
#if 0
            printf("Got available after populate: (%zu, %zu)\n", i, ptrAt);
#endif
            return ptrAt;
        }
    }

    assert(NULL && "Didn't find free after repopulate!");

    __builtin_unreachable();
}

/* ====================================================================
 * Access
 * ==================================================================== */
DK_STATIC size_t grow(multilru *mlru) {
    /* REFACTOR:
     * The lruEntry space is currently just a linear array.
     *
     * This could be bad because we'll be stressing the allocator
     * at higher LRU count ranges.
     *
     * On the other hand, allocating a million entries in our LRU
     * array is just 16 MB, which shouldn't be too difficult on
     * any allocator (though, a billion entires is 16 GB).
     *
     * Profile and verify if okay or if we need multiple levels of
     * allocations here.
     */
    const size_t originalSize =
        sizeof(*mlru->entries) * (mlru->highestAllocated);
    const size_t growTo = jebufSizeAllocation(originalSize * 2);

#if 0
    printf("GROWING FROM %zu TO %zu\n", originalSize, growTo);
    printf("GROWING FROM C%zu TO C%zu\n", originalSize / sizeof(*mlru->entries),
           growTo / sizeof(*mlru->entries));
#endif

    if (!mlru->entries) {
        /* For initial allocation, we start at base conditions instead of using
         * math based on highestAllocated */
        mlru->entries = zcalloc(1, growTo);
        markFreeStarting(mlru, 1);
    } else {
        mlru->entries = zrealloc(mlru->entries, growTo);
        /* Need to zero out or newly grown memory so it is detected as UNUSED;
         * (i.e. directly sets ptr->isPopulated = false)*/
        memset(mlru->entries + mlru->highestAllocated, 0,
               growTo - originalSize);

        /* Since we have new unisPopulated entries, attempt to add them to
         * our free list (since we probably grow only when we run out of
         * existing entries, meaning we immediately need more available
         * entries!) */
        markFreeStarting(mlru, mlru->highestAllocated);
    }

    /* Note: we use 'highestAllocated' as an upper bound, so we do
     * minus one here so we don't have to do it everywhere else
     * all the time */
    mlru->highestAllocated = (growTo / sizeof(*mlru->entries));

    return growTo;
}

size_t multilruBytes(const multilru *mlru) {
    return (mlru->highestAllocated) * sizeof(*mlru->entries);
}

size_t multilruCount(const multilru *mlru) {
    return mlru->count;
}

size_t multilruCapacity(const multilru *mlru) {
    return mlru->highestAllocated;
}

DK_INLINE_ALWAYS lruEntry *entryNew(multilru *mlru) {
    /* If we're out of free space, grow slots... */
    if (mlru->count + mlru->maxLevels >= (mlru->highestAllocated - 1)) {
        grow(mlru);
    }

    /* We have at least one free slot somewhere. */
    const multilruPtr newer = getAvailable(mlru);

    lruEntry *newE = entry(newer);
    assert(!newE->isPopulated);

    newE->isPopulated = true;
    return newE;
}

#define getLowest(mlru) (entry((mlru)->lowest))

/* Take pointer inside 'mlru->entries' and make it an offset index instead. */
#define ARRAY_PTR_TO_ARRAY_OFFSET(mlru, ptr)                                   \
    (multilruPtr)((((uintptr_t)ptr) - ((uintptr_t)(mlru)->entries)) /          \
                  sizeof(*mlru->entries))

/* ====================================================================
 * Creation / Init / Destruction / Free
 * ==================================================================== */
multilru *multilruNew(void) {
    return multilruNewWithLevels(7);
}

multilru *multilruNewWithLevels(const size_t maxLevels) {
    return multilruNewWithLevelsCapacity(maxLevels, 2048);
}

multilru *multilruNewWithLevelsCapacity(const size_t maxLevels,
                                        const size_t startCapacity) {
    multilru *mlru = zcalloc(1, sizeof(*mlru));
    mlru->highestAllocated = startCapacity / 2;

    /* doubles highestAllocated (also rounds up to fill allocation class) */
    (void)grow(mlru);

    mlru->level = zcalloc(maxLevels, sizeof(*mlru->entries));
    mlru->maxLevels = maxLevels;

    for (size_t i = 1; i <= mlru->maxLevels; i++) {
        /* if 'grow()' popualtes our free list correctly,
         * 'entryNew()' will return entries in 'i' order */
        lruEntry *e = entryNew(mlru);
        assert(ARRAY_PTR_TO_ARRAY_OFFSET(mlru, e) == i);

        if (i < mlru->maxLevels) {
            e->next = i + 1;
        }

        e->prev = i - 1;

        e->isHeadNode = true;
        e->isPopulated = true;
        mlru->level[i - 1] = ARRAY_PTR_TO_ARRAY_OFFSET(mlru, e);
        assert(mlru->level[i - 1]);
    }

    return mlru;
}

void multilruFree(multilru *mlru) {
    if (mlru) {
        zfree(mlru->entries);
        zfree(mlru->level);
        zfree(mlru);
    }
}

/* ====================================================================
 * Operational Stability Helpers
 * ==================================================================== */
DK_INLINE_ALWAYS bool assignNextLowest(multilru *mlru, const bool forward) {
    const multilruPtr originalLowest = mlru->lowest;
    const lruEntry *currentLowest = entry(originalLowest);
    assert(!currentLowest->isHeadNode);
    assert(mlru->count);

    assert(entry(currentLowest->prev)->isHeadNode || currentLowest->prev == 0);

    removeEntryFromCurrentPosition(mlru, currentLowest);

    /* Check if NEXT entry is a valid candidate for being LOWEST */
    multilruPtr next = forward ? currentLowest->next : currentLowest->prev;
    lruEntry *low = entry(next);

    while (next) {
        /* Assign new lowest to NEXT only if NEXT is *not* a head marker. */
        if (!low->isHeadNode) {
            /* Now mlru will be:
             * [next -> H0 -> H1 -> H2 -> ...] */
            assignLowest(next);

            /* all done! */
            return true;
        }

        next = forward ? low->next : low->prev;
        low = entry(next);
    }

    assert((mlru->count == 1 && !mlru->lowest) || mlru->lowest);

    return false;
}

DK_STATIC void removeEntryFromCurrentPosition(multilru *mlru,
                                              const lruEntry *entryCurrent) {
    assert(!entryCurrent->isHeadNode);

    lruEntry *currentPrev = NULL;
    if (entryCurrent->prev) {
        currentPrev = entry(entryCurrent->prev);
        currentPrev->next = entryCurrent->next;
    }

    lruEntry *currentNext = NULL;
    if (entryCurrent->next) {
        currentNext = entry(entryCurrent->next);
        currentNext->prev = entryCurrent->prev;
    }
}

DK_INLINE_ALWAYS void moveToHead(multilru *mlru, const uint32_t level,
                                 const multilruPtr current,
                                 const bool isInsert) {
    /* Never move head markers. */
    assert(current >= mlru->maxLevels);

    /* Level Markers are non-value entires and adding something
     * to the head of a level means placing it *before* the level
     * marker. */
    lruEntry *levelMarker = entry(mlru->level[level]);
    lruEntry *entryCurrent = entry(current);

    if (!mlru->count) {
        assert(!mlru->lowest);
        assert(current);
        assert(entryCurrent->prev == 0);
        assert(mlru->level[level]);

        /* If no count, we need to re-init the mlru by
         * setting a new lowest and re-setting the level[0]
         * prev details. */
        assignLowest(current);
        entryCurrent->currentLevel = level;
        entryCurrent->next = mlru->level[level];
        levelMarker->prev = current;
        return;
    }

    /* Cut out of existing linked chain:
     * e.g. [A] <-> [B] <-> [C]
     *      [A] <-> [C] */
    if (!isInsert) {
        removeEntryFromCurrentPosition(mlru, entryCurrent);
    }

    /* If we just deleted the lowest entry AND IT HAS NO REPLACEMENT,
     * we are the only node in the multilru and we become the lowest
     * element again.
     *
     * This isn't integrated into 'removeEntryFromCurrentPosition' because
     * 'remove' is an implicit delete/CONSUME operation, but here we
     * take the removed node and just re-insert it back into the list
     * at a higher position.
     *
     * Intended consistency remains conserved. */
    if (!mlru->lowest || levelMarker->prev == 0) {
        assignLowest(current);
    }

    /* Add to head of level:
     * e.g. [A] <-> [LEVEL]
     *      [A] <-> [B] <-> [LEVEL] */
    entryCurrent->currentLevel = level;
    entryCurrent->prev = levelMarker->prev;
    entryCurrent->next = mlru->level[level];

    if (levelMarker->prev) {
        lruEntry *entryPrev = entry(levelMarker->prev);
        entryPrev->next = current;
    }

    levelMarker->prev = current;
}

/* ====================================================================
 * Insert
 * ==================================================================== */
multilruPtr multilruInsert(multilru *mlru) {
    /* Create new entry to be head of level pointing prev to current head */
    lruEntry *newHead = entryNew(mlru);

    multilruPtr newPtr = ARRAY_PTR_TO_ARRAY_OFFSET(mlru, newHead);
    moveToHead(mlru, 0, newPtr, true);

    mlru->count++;
    return newPtr;
}

/* ====================================================================
 * Increase
 * ==================================================================== */
void multilruIncrease(multilru *mlru, const multilruPtr currentPtr) {
    /* If already at highest level, move to top of current level */
    size_t level = entry(currentPtr)->currentLevel + 1;
    if (level > mlru->maxLevels - 1) {
        level = mlru->maxLevels - 1;
    }

    /* If moving LOWEST entry, need to assign a new lowest.
     * Does NOT consume/delete entry, just moves it. */
    if (currentPtr == mlru->lowest) {
        /* This ptr is the current lowest, so we must decide:
         *  Do we need a new lowest?
         *
         * If a layout looks like this, we don't need a new lowest:
         *  [H1 -> ptr -> H2 -> H3 -> ptr2 -> H4]
         *
         * We just keep 'ptr' as lowest even thing we're increasing
         * the level.
         *
         * But, if layout looks like this, we do need a new lowest:
         *  [H1 -> ptr -> H2 -> ptr2, ptr3 -> H3]
         * Because after this Increase, the result will be:
         *  [H1 -> H2 -> ptr2, ptr3, ptr -> H3]
         * So ptr2 becomes the new lowest.
         */

        /* If we have a next entry, we know we can populate new lowest. */
        const lruEntry *restrict currentEntry = entry(currentPtr);
        const lruEntry *restrict currentEntryNext = entry(currentEntry->next);
        if (!currentEntryNext->isHeadNode) {
            assignNextLowest(mlru, true);
        } else {
            /* After this increase, we MAY still be the lowest entry!
             *
             * We check by:
             *  - for the level we're being promoted to:
             *      - is it empty? if so, we are still the lowest.
             *      - if not, we need to iterate through head nodes to find
             *        our next actually lowest entry. */
            const lruEntry *restrict targetLevel = entry(mlru->level[level]);
            const lruEntry *restrict targetLevelPrev = entry(targetLevel->prev);
            if (!targetLevelPrev->isHeadNode) {
                const bool assigned = assignNextLowest(mlru, true);
                (void)assigned;
                if (mlru->count > 1) {
                    assert(assigned);
                } else {
                    assert(!assigned);
                }
            }
        }
    }

    /* Make 'currentPtr' new head of '*level' */
    moveToHead(mlru, level, currentPtr, false);
}

/* ====================================================================
 * Delete Minimum
 * ==================================================================== */
/* Delete minimum only removes the lowest element */
/* TODO: RemoveMinN - get up to N entries to remove all at once */
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *atomRef) {
    /* read start
     * set start == start->next
     * remove original start */

    if (!mlru->lowest) {
        assert(mlru->count == 0);
        return false;
    }

    assert(mlru->count > 0);

    const multilruPtr originalLowest = mlru->lowest;
    lruEntry *least = getLowest(mlru);

    if (atomRef) {
        *atomRef = ARRAY_PTR_TO_ARRAY_OFFSET(mlru, least);
    }

    /* Verify least element previous is EITHER
     * 0 meaning it's the absolute lowest OR
     * it's also okay for it to be a HEAD MARKER
     * meaning the LEAST ELEMENT is NOT in the
     * lowest access class, but is a few levels up,
     * and the previous entry is a previous level head */

    /* Verify lowest 'prev' is EITHER:
     *  - zero because if we're the lowest element, nothing can be 'prev' to
     * us.
     *  - OR, a head node because we're the least element but were
     * previously Increased but no lesser used elements are below us. */
    assert(least->prev == 0 || entry(least->prev)->isHeadNode);

    if (!assignNextLowest(mlru, true)) {
        /* If we didn't change the lowest entry, reset lowest to zero
         * because we're going to delete the current lowest and apparently
         * no other elements exist! */
        assert(originalLowest == mlru->lowest);

        /* Count will be decremented to zero at the free below */
        assert(mlru->count == 1);

        assignLowest(0);
    }

    markFreeCleanup(mlru, originalLowest);

    return true;
}

/* ====================================================================
 * Delete
 * ==================================================================== */
/* Delete removes 'ptr' from any position in the LRU */
void multilruDelete(multilru *mlru, const multilruPtr ptr) {
    assert(ptr >= mlru->maxLevels);

    if (ptr == mlru->lowest) {
        assert(mlru->count > 0);
        multilruRemoveMinimum(mlru, NULL);
        /* RemoveMinimum adjusts count for us, so we MUST NOT
         * decrement mlru->count here if we use RemoveMinimum */
    } else {
        lruEntry *e = entry(ptr);
        assert(!e->isHeadNode);

        /* If pointer doesn't exist, don't operate on zero data */
        if (!e->isPopulated) {
            return;
        }

        removeEntryFromCurrentPosition(mlru, e);
        markFreeCleanup(mlru, ptr);
    }

#ifndef NDEBUG
    if (!mlru->count) {
        assert(!mlru->lowest);
    } else {
        assert(mlru->lowest);
    }
#endif
}

/* ====================================================================
 * Maint
 * ==================================================================== */
bool multilruMaintain(multilru *mlru) {
    for (size_t i = 0; i < mlru->highestAllocated; i++) {
    }

    return true;
}

void multilruGetNLowest(multilru *mlru, multilruPtr N[], size_t n) {
    multilruPtr current = mlru->lowest;

    for (size_t i = 0; i < n && current; i++) {
        N[i] = current;

        /* Walk forward */
        lruEntry *nextNode = entry(entry(current)->next);

        /* Skip over head node pointers if we hit them */
        while (nextNode->isHeadNode) {
            nextNode = entry(nextNode->next);
        }

        current = ARRAY_PTR_TO_ARRAY_OFFSET(mlru, nextNode);
    }
}

void multilruGetNHighest(multilru *mlru, multilruPtr N[], size_t n) {
    /* Start from the head node of the highest level */
    multilruPtr current = mlru->level[mlru->maxLevels - 1];

    for (size_t i = 0; i < n && current; i++) {
        /* Walk backward */
        lruEntry *prevNode = entry(entry(current)->prev);

        /* Skip over head node pointers if we hit them */
        while (prevNode->isHeadNode) {
            prevNode = entry(prevNode->prev);
        }

        current = N[i] = ARRAY_PTR_TO_ARRAY_OFFSET(mlru, prevNode);
    }
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include <stdio.h>

/* ====================================================================
 * Report
 * ==================================================================== */
ssize_t multilruTraverseSize(const multilru *mlru) {
    multilruPtr current = mlru->lowest;

    /* If minimum isn't on our lowest list, start iterating
     * at lowest list anyway. */
    if (entry(current)->currentLevel != 0) {
        current = 1;
    }

    ssize_t foundElements = 0;
    if (current) {
        do {
            const lruEntry *c = entry(current);
            current = c->next;
            foundElements++;
        } while (current);

        foundElements -= mlru->maxLevels;
    }

    return foundElements;
}

void multilruRepr(const multilru *mlru) {
    multilruPtr current = mlru->lowest;

    /* If minimum isn't on our lowest list, start iterating
     * at lowest list anyway. */
    if (entry(current)->currentLevel > 1) {
        current = 1;
    }

    printf("{count {used %zu} {capacity %zu}} {lowest %zu} {bytes {allocated "
           "%zu}}\n",
           multilruCount(mlru), multilruCapacity(mlru), mlru->lowest,
           multilruBytes(mlru));

    printf("{%zu} ", mlru->lowest);

    for (uint32_t i = 0; i < mlru->maxLevels; i++) {
        lruEntry *e = entry(mlru->level[i]);
        lruEntry *prev = entry(e->prev);
        printf("[%" PRIu32 "] -> %d; ", i, prev->isHeadNode ? 0 : e->prev);
    }

    printf("\n");

    // size_t foundElements = 0;
    do {
        const lruEntry *c = entry(current);
        printf("(%s%zu) -> ", c->isHeadNode ? "[H]" : "", (size_t)current);
        //        assert(count++ <= mlru->count);
        current = c->next;
        // foundElements++;
    } while (current);

    printf("{count %zu}\n", mlru->count);
    printf("\n");

#if 0
    printf("{free...}\n");
    for (size_t i = 0; i < COUNT_ARRAY(mlru->freePosition); i++) {
        printf("[%d] -> %d; ", i, mlru->freePosition[i]);
    }
    printf("\n");
#endif
}

/* ====================================================================
 * Testing
 * ==================================================================== */
int multilruTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

#if 1
    TEST("create empty") {
        multilru *mlru = multilruNew();

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 1") {
        multilru *mlru = multilruNew();
        multilruInsert(mlru);

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("increase lowest the insert new lowest") {
        multilru *mlru = multilruNew();

        /* mlru is empty here */
        /* [] */
        assert(mlru->lowest == 0);
        assert(multilruTraverseSize(mlru) == 0);

        const multilruPtr pa = multilruInsert(mlru);
        multilruRepr(mlru);
        /* [pa -> H1] */

        multilruIncrease(mlru, pa);
        multilruRepr(mlru);
        /* [H1 -> pa -> H2] */

        multilruIncrease(mlru, pa);
        multilruRepr(mlru);
        /* [H2 -> pa -> H3] */

        /* mlru lowest is 'pa' now */
        assert(mlru->lowest == pa);
        assert(multilruTraverseSize(mlru) == 1);

        const multilruPtr pa2 = multilruInsert(mlru);
        multilruRepr(mlru);
        /* [pa2 -> H1 -> H2 -> pa -> H3] */

        /* mlru lowest is 'pa2' now */
        assert(mlru->lowest == pa2);
        assert(multilruTraverseSize(mlru) == 2);

        multilruIncrease(mlru, pa2);
        multilruRepr(mlru);
        /* [H1 -> pa2 -> H2 -> pa -> H3] */

        /* mlru lowest is still 'pa2' */
        assert(mlru->lowest == pa2);
        assert(multilruTraverseSize(mlru) == 2);

        multilruDelete(mlru, pa);
        multilruRepr(mlru);
        /* [H1 -> pa2 -> H2] */

        assert(mlru->lowest == pa2);
        assert(multilruTraverseSize(mlru) == 1);

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("increase lowest higher the insert new lowest") {
        multilru *mlru = multilruNew();

        /* mlru is empty here */
        /* [] */
        assert(mlru->lowest == 0);
        assert(multilruTraverseSize(mlru) == 0);

        const multilruPtr pa = multilruInsert(mlru);
        multilruRepr(mlru);
        /* [pa -> H1] */

        multilruIncrease(mlru, pa);
        multilruRepr(mlru);
        /* [H1 -> pa -> H2] */

        multilruIncrease(mlru, pa);
        multilruRepr(mlru);
        /* [H2 -> pa -> H3] */

        /* mlru lowest is 'pa' now */
        assert(mlru->lowest == pa);
        assert(multilruTraverseSize(mlru) == 1);

        const multilruPtr pa2 = multilruInsert(mlru);
        multilruRepr(mlru);
        /* [pa2 -> H1 -> H2 -> pa -> H3] */

        /* mlru lowest is 'pa2' now */
        assert(mlru->lowest == pa2);
        assert(multilruTraverseSize(mlru) == 2);

        multilruIncrease(mlru, pa2);
        multilruRepr(mlru);
        /* [H1 -> pa2 -> H2 -> pa -> H3] */

        /* mlru lowest is still 'pa2' */
        assert(mlru->lowest == pa2);
        assert(multilruTraverseSize(mlru) == 2);

        multilruDelete(mlru, pa2);
        multilruRepr(mlru);
        /* [H1 -> H2 -> pa -> H3] */

        assert(mlru->lowest == pa);
        assert(multilruTraverseSize(mlru) == 1);

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 4 delete 4") {
        multilru *mlru = multilruNew();
        const multilruPtr pa = multilruInsert(mlru);
        multilruInsert(mlru);
        multilruInsert(mlru);
        multilruInsert(mlru);

        multilruDelete(mlru, pa);
        multilruDelete(mlru, pa + 1);
        multilruDelete(mlru, pa + 2);
        multilruDelete(mlru, pa + 3);

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 1 and increment") {
        multilru *mlru = multilruNew();
        const multilruPtr mp = multilruInsert(mlru);
        multilruRepr(mlru);

        multilruIncrease(mlru, mp);

        multilruRepr(mlru);
        assert(mlru->lowest == mp);

        multilruFree(mlru);
    }

    TEST("create populate 2 and increment 1st") {
        multilru *mlru = multilruNew();
        const multilruPtr mp = multilruInsert(mlru);
        multilruInsert(mlru);

        multilruIncrease(mlru, mp);

        multilruRepr(mlru);

        /* Verify the lowest element is the NOT increment one */
        assert(mlru->lowest == (mp + 1));

        multilruFree(mlru);
    }

    TEST("create populate 2 and increment 2nd") {
        multilru *mlru = multilruNew();
        multilruInsert(mlru);
        const multilruPtr mp = multilruInsert(mlru);

        multilruIncrease(mlru, mp);

        multilruRepr(mlru);

        /* Verify lowest element is the NOT incremented one */
        assert(mlru->lowest == (mp - 1));

        multilruFree(mlru);
    }

    TEST("create populate 20") {
        multilru *mlru = multilruNew();
        for (int i = 0; i < 20; i++) {
            multilruInsert(mlru);
        }

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 20 increase 1") {
        multilru *mlru = multilruNew();
        multilruPtr holder[20] = {0};
        for (int i = 0; i < 20; i++) {
            holder[i] = multilruInsert(mlru);
        }

        multilruIncrease(mlru, holder[7]);

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 20 increase 10") {
        multilru *mlru = multilruNew();
        multilruPtr holder[20] = {0};
        for (int i = 0; i < 20; i++) {
            holder[i] = multilruInsert(mlru);
        }

        for (int i = 1; i <= 10; i++) {
            multilruIncrease(mlru, holder[i]);
        }

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 20 increase 10 10 times") {
        multilru *mlru = multilruNew();
        multilruPtr holder[20] = {0};
        for (int i = 0; i < 20; i++) {
            holder[i] = multilruInsert(mlru);
        }

        for (int j = 1; j <= 10; j++) {
            for (int i = 0; i < 10; i++) {
                multilruIncrease(mlru, holder[i]);
            }
        }

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate 20 remove all") {
        multilru *mlru = multilruNew();
        multilruPtr holder[20] = {0};
        for (int i = 0; i < 20; i++) {
            holder[i] = multilruInsert(mlru);
        }

        for (int i = 0; i < 20; i++) {
            multilruPtr val;
            const bool removed = multilruRemoveMinimum(mlru, &val);
            assert(removed);
            assert(val == holder[i]);
        }

        multilruRepr(mlru);
        multilruFree(mlru);
    }

    TEST("create populate for growth") {
        multilru *mlru = multilruNew();
        for (int i = 0; i < 768; i++) {
            multilruInsert(mlru);
        }
        multilruFree(mlru);
    }

    TEST("create populate for remove, re-insert") {
        multilru *mlru = multilruNew();
        for (int i = 0; i < 768; i++) {
            multilruInsert(mlru);
        }

        for (int j = 0; j < 300; j++) {
            // multilruPtr prevRemoved = 0;
            for (int i = 0; i < 200; i++) {
                multilruPtr val;
                const bool removed = multilruRemoveMinimum(mlru, &val);
                assert(removed);
                // prevRemoved = val;
            }

            for (int i = 0; i < 768; i++) {
                multilruInsert(mlru);
            }
        }

        multilruPtr lowest[7] = {0};
        multilruGetNLowest(mlru, lowest, COUNT_ARRAY(lowest));

        for (size_t i = 0; i < COUNT_ARRAY(lowest); i++) {
            printf("Lowest %zu: %zu\n", i, lowest[i]);
        }

        multilruPtr highest[7] = {0};
        multilruGetNHighest(mlru, highest, COUNT_ARRAY(highest));

        for (size_t i = 0; i < COUNT_ARRAY(highest); i++) {
            printf("Highest %zu: %zu\n", i, highest[i]);
        }

        printf("Total bytes used: %zu for %zu entries with %zu capacity\n",
               multilruBytes(mlru), multilruCount(mlru),
               multilruCapacity(mlru));
        multilruFree(mlru);
    }

    TEST("bigger situations") {
        multilru *mlru = multilruNew();

        multilruRepr(mlru);

        const size_t maxInsert = 768;
        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            assert(ptrd == i + mlru->maxLevels + 1);
        }

        assert(mlru->count == maxInsert);
        assert(multilruTraverseSize(mlru) == maxInsert);

        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            assert(ptrd == i + maxInsert + mlru->maxLevels + 1);
            multilruIncrease(mlru, ptrd);
        }

        assert(mlru->count == maxInsert * 2);
        assert(multilruTraverseSize(mlru) == maxInsert * 2);

        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            assert(ptrd == i + maxInsert * 2 + mlru->maxLevels + 1);
            multilruIncrease(mlru, ptrd);
            multilruIncrease(mlru, ptrd);
        }

        assert(mlru->count == maxInsert * 3);
        assert(multilruTraverseSize(mlru) == maxInsert * 3);

        /* Delete from high to low */
        for (ssize_t j = maxInsert * 3 - 1; j >= 0; j--) {
            const multilruPtr mp = j + mlru->maxLevels + 1;
            assert(mlru->count > 0);
            multilruDelete(mlru, mp);
        }

        multilruRepr(mlru);
        assert(mlru->count == 0);
        assert(mlru->lowest == 0);
        assert(entry(1)->prev == 0);
        if (mlru->maxLevels > 1) {
            assert(entry(1)->next == 2);
        }
        assert(multilruTraverseSize(mlru) == 0);

        /* Repopulate */
        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            (void)ptrd;
        }

        assert(mlru->count == maxInsert);
        assert(multilruTraverseSize(mlru) == maxInsert);

        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            multilruIncrease(mlru, ptrd);
        }

        assert(mlru->count == maxInsert * 2);
        assert(multilruTraverseSize(mlru) == maxInsert * 2);

        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);
            multilruIncrease(mlru, ptrd);
            multilruIncrease(mlru, ptrd);
        }

        assert(mlru->count == maxInsert * 3);
        assert(multilruTraverseSize(mlru) == maxInsert * 3);

        /* Delete from low to high until empty */
        size_t j = 0;
        while (mlru->count) {
            const multilruPtr mp = j++ + mlru->maxLevels + 1;
            multilruDelete(mlru, mp);
            assert(multilruTraverseSize(mlru) == (ssize_t)mlru->count);
        }

        assert(mlru->count == 0);

        multilruRepr(mlru);
        printf("Traversed Size: %zd\n", multilruTraverseSize(mlru));
        assert(multilruTraverseSize(mlru) == 0);
        printf("Total bytes used: %zu for %zu entries with %zu capacity\n",
               multilruBytes(mlru), multilruCount(mlru),
               multilruCapacity(mlru));
        multilruFree(mlru);
    }
#endif

    TEST("randomizer situations") {
        multilru *mlru = multilruNew();

        const size_t maxInsert = 16;
        for (size_t i = 0; i < maxInsert; i++) {
            const multilruPtr ptrd = multilruInsert(mlru);

            printf("Inserted %zu\n", ptrd);
            multilruRepr(mlru);

            multilruIncrease(mlru, ptrd);

            printf("Incremented %zu\n", ptrd);
            multilruRepr(mlru);

            assert(ptrd == i + mlru->maxLevels + 1);
        }

        assert(mlru->count == maxInsert);
        assert(multilruTraverseSize(mlru) == (ssize_t)mlru->count);

        /* Delete in random order */
        size_t j = 0;
        while (mlru->count) {
            const multilruPtr mp = j++ + mlru->maxLevels + 1;
            printf("Deleting %zu\n", mp);
            multilruDelete(mlru, mp);
            multilruRepr(mlru);
        }

        multilruRepr(mlru);
        assert(mlru->count == 0);
        assert(mlru->lowest == 0);
        assert(entry(1)->prev == 0);
        if (mlru->maxLevels > 1) {
            assert(entry(1)->next == 2);
        }

        assert(multilruTraverseSize(mlru) == 0);
        multilruFree(mlru);
    }

    TEST("multilruGetNHighest correctness verification") {
        multilru *mlru = multilruNew();

        /* Insert 100 entries and promote some to different levels */
        multilruPtr ptrs[100] = {0};
        for (size_t i = 0; i < 100; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Promote entries to various levels:
         * ptrs[90-99] get 6 increases -> level 6 (highest)
         * ptrs[80-89] get 5 increases -> level 5
         * ptrs[70-79] get 4 increases -> level 4
         * etc. */
        for (size_t i = 90; i < 100; i++) {
            for (size_t j = 0; j < 6; j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }
        for (size_t i = 80; i < 90; i++) {
            for (size_t j = 0; j < 5; j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }
        for (size_t i = 70; i < 80; i++) {
            for (size_t j = 0; j < 4; j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }

        /* Get the N highest entries */
        multilruPtr highest[20] = {0};
        multilruGetNHighest(mlru, highest, 20);

        /* Verify we get the entries from highest levels first */
        printf("Highest entries after promotion:\n");
        for (size_t i = 0; i < 20; i++) {
            printf("  highest[%zu] = %zu\n", i, highest[i]);
            /* First 10 should be from ptrs[90-99] (highest level) */
            if (i < 10) {
                bool found = false;
                for (size_t k = 90; k < 100; k++) {
                    if (highest[i] == ptrs[k]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ERR("Expected highest[%zu] to be from level 6 entries", i);
                }
            }
        }

        /* Also verify GetNLowest */
        multilruPtr lowest[20] = {0};
        multilruGetNLowest(mlru, lowest, 20);

        printf("Lowest entries:\n");
        for (size_t i = 0; i < 10; i++) {
            printf("  lowest[%zu] = %zu\n", i, lowest[i]);
        }

        multilruFree(mlru);
    }

    TEST("high-scale stress test (100k entries)") {
        multilru *mlru = multilruNewWithLevelsCapacity(7, 65536);

        const size_t numEntries = 100000;
        multilruPtr *ptrs = zcalloc(numEntries, sizeof(multilruPtr));

        /* Insert all entries */
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            ptrs[i] = multilruInsert(mlru);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        assert(multilruCount(mlru) == numEntries);
        printf("Inserted %zu entries in %.3f ms (%.0f/sec)\n", numEntries,
               insertNs / 1e6, numEntries / (insertNs / 1e9));

        /* Increase entries randomly */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            size_t increaseCount = i % 7; /* 0-6 increases per entry */
            for (size_t j = 0; j < increaseCount; j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }
        int64_t increaseNs = timeUtilMonotonicNs() - startNs;
        printf("Increased entries in %.3f ms\n", increaseNs / 1e6);

        /* Query highest and lowest */
        multilruPtr highest[100] = {0};
        multilruPtr lowest[100] = {0};

        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < 1000; i++) {
            multilruGetNHighest(mlru, highest, 100);
            multilruGetNLowest(mlru, lowest, 100);
        }
        int64_t queryNs = timeUtilMonotonicNs() - startNs;
        printf(
            "2000 queries (100 entries each) in %.3f ms (%.0f queries/sec)\n",
            queryNs / 1e6, 2000.0 / (queryNs / 1e9));

        /* Remove minimum entries */
        startNs = timeUtilMonotonicNs();
        size_t removeCount = numEntries / 2;
        for (size_t i = 0; i < removeCount; i++) {
            multilruPtr removed;
            bool ok = multilruRemoveMinimum(mlru, &removed);
            assert(ok);
            (void)ok;
        }
        int64_t removeNs = timeUtilMonotonicNs() - startNs;
        printf("Removed %zu entries via RemoveMinimum in %.3f ms (%.0f/sec)\n",
               removeCount, removeNs / 1e6, removeCount / (removeNs / 1e9));

        assert(multilruCount(mlru) == numEntries - removeCount);

        /* Memory efficiency report */
        size_t bytesUsed = multilruBytes(mlru);
        size_t entriesRemaining = multilruCount(mlru);
        printf("Memory: %zu bytes for %zu entries (%.2f bytes/entry)\n",
               bytesUsed, entriesRemaining,
               entriesRemaining > 0 ? (double)bytesUsed / entriesRemaining : 0);

        zfree(ptrs);
        multilruFree(mlru);
    }

    TEST("random delete stress test") {
        multilru *mlru = multilruNew();

        const size_t numEntries = 10000;
        multilruPtr *ptrs = zcalloc(numEntries, sizeof(multilruPtr));

        /* Insert and randomly promote */
        for (size_t i = 0; i < numEntries; i++) {
            ptrs[i] = multilruInsert(mlru);
            size_t promotes = random() % 8;
            for (size_t j = 0; j < promotes; j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }

        /* Delete in random order */
        for (size_t i = numEntries; i > 0; i--) {
            size_t idx = random() % i;
            multilruDelete(mlru, ptrs[idx]);
            /* Move last to current position */
            ptrs[idx] = ptrs[i - 1];
        }

        assert(multilruCount(mlru) == 0);
        assert(multilruTraverseSize(mlru) == 0);

        zfree(ptrs);
        multilruFree(mlru);
    }

    TEST("performance benchmark summary") {
        printf("\n=== MULTILRU PERFORMANCE SUMMARY ===\n");

        /* Benchmark insert */
        multilru *mlru = multilruNewWithLevelsCapacity(7, 131072);
        const size_t benchCount = 500000;

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            multilruInsert(mlru);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark increase (promotes to higher level) */
        multilruPtr testPtr = mlru->lowest;
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < 100000 && testPtr; i++) {
            multilruIncrease(mlru, testPtr);
        }
        int64_t increaseNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark remove minimum */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < 100000; i++) {
            multilruPtr removed;
            multilruRemoveMinimum(mlru, &removed);
        }
        int64_t removeNs = timeUtilMonotonicNs() - startNs;

        printf("Insert rate:   %.0f ops/sec (%.1f ns/op)\n",
               benchCount / (insertNs / 1e9), (double)insertNs / benchCount);
        printf("Increase rate: %.0f ops/sec (%.1f ns/op)\n",
               100000.0 / (increaseNs / 1e9), (double)increaseNs / 100000);
        printf("Remove rate:   %.0f ops/sec (%.1f ns/op)\n",
               100000.0 / (removeNs / 1e9), (double)removeNs / 100000);
        printf("Memory used:   %zu bytes for %zu entries (%.2f bytes/entry)\n",
               multilruBytes(mlru), multilruCount(mlru),
               multilruCount(mlru) > 0
                   ? (double)multilruBytes(mlru) / multilruCount(mlru)
                   : 0);
        printf("=====================================\n\n");

        multilruFree(mlru);
    }

    TEST_FINAL_RESULT;
}
#endif
