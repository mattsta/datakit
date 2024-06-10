#include "multilistMedium.h"
#include "multilistMediumInternal.h"

/* ====================================================================
 * Management Macros
 * ==================================================================== */

/* Easy way to get nodeIdx of active tail:
 *   - use fl[1] if tail has elements, else use 0 */
#define TAIL_INDEX_FROM_COUNT(count) ((count) > 0)

#define MF0(m) ((m)->fl[0])
#define MF1(m) ((m)->fl[1])
#define F0 MF0(ml)
#define F1 MF1(ml)

#define swapF()                                                                \
    do {                                                                       \
        flex *tmp = F0;                                                        \
        F0 = F1;                                                               \
        F1 = tmp;                                                              \
    } while (0)

/* ====================================================================
 * Create
 * ==================================================================== */
static multilistMedium *multilistMediumCreateContainer(void) {
    multilistMedium *ml = zcalloc(1, sizeof(*ml));
    return ml;
}

multilistMedium *multilistMediumCreate(void) {
    multilistMedium *ml = multilistMediumCreateContainer();
    F0 = flexNew();
    F1 = flexNew();
    return ml;
}

/* ====================================================================
 * Copy
 * ==================================================================== */
multilistMedium *multilistMediumDuplicate(multilistMedium *orig) {
    multilistMedium *ml = multilistMediumCreate();
    F0 = flexDuplicate(MF0(orig));
    F1 = flexDuplicate(MF1(orig));
    return ml;
}

/* ====================================================================
 * Free
 * ==================================================================== */
void multilistMediumFree(multilistMedium *ml) {
    if (ml) {
        flexFree(F0);
        flexFree(F1);
        zfree(ml);
    }
}

/* ====================================================================
 * Metadata
 * ==================================================================== */
size_t multilistMediumCount(const multilistMedium *ml) {
    return flexCount(F0) + flexCount(F1);
}

size_t multilistMediumBytes(const multilistMedium *ml) {
    return flexBytes(F0) + flexBytes(F1);
}

/* ====================================================================
 * Bulk Operations
 * ==================================================================== */
void multilistMediumAppendFlex(multilistMedium *ml, const flex *fl) {
    /* If head is tail, copy 'fl' as new tail. */
    const size_t countF0 = flexCount(F0);
    const size_t countF1 = flexCount(F1);

    /* If no current data, split 'fl' in half */
    if (countF0 == 0 && countF1 == 0) {
        /* Replace f0, f1 with 'fl' split in half */
        flexFree(F0);
        flexFree(F1);
        F0 = flexDuplicate(fl);
        F1 = flexSplit(&F0, 1);
    } else {
        /* else, append 'fl' to tail. */
        /* TODO:
         *   - can optimize for re-balancing F0 and F1
         *     after this to make sure F1 didn't just get
         *     100 elements while F0 may have 1 element. */
        flexBulkAppendFlex(&F1, fl);
    }
}

void multilistMediumAppendValuesFromFlex(multilistMedium *ml, const flex *fl) {
    multilistMediumAppendFlex(ml, fl);
}

static inline void multilistMediumInitFromFlexConsume_(multilistMedium *ml,
                                                       flex *fl) {
    if (flexCount(fl) > 1) {
        F0 = fl;
        F1 = flexSplit(&F0, 1);
    } else {
        /* else, we are creating medium from only one element,
         * and F0 must always be populated, so just make 'fl' F0
         * and add an empty flex as F1. */
        F0 = fl;
        F1 = flexNew();
    }
}

/* Create new flex by splitting 'fl' in half.
 * Modifies 'fl' in place. */
multilistMedium *multilistMediumNewFromFlexConsume(flex *fl) {
    multilistMedium *ml = multilistMediumCreateContainer();

    multilistMediumInitFromFlexConsume_(ml, fl);
    return ml;
}

multilistMedium *multilistMediumNewFromFlexConsumeGrow(void *_ml, flex *fl) {
    multilistMedium *ml = zrealloc(_ml, sizeof(*ml));

    multilistMediumInitFromFlexConsume_(ml, fl);
    return ml;
}

/* Create new flex by splitting 'fl' in half.
 * Duplicates 'fl' so original remains untouched. */
multilistMedium *multilistMediumNewFromFlex(const flex *fl) {
    return multilistMediumNewFromFlexConsume(flexDuplicate(fl));
}

/* ====================================================================
 * Insert with cursor
 * ==================================================================== */
void multilistMediumInsertByTypeBefore(multilistMedium *ml,
                                       const multilistEntry *entry,
                                       const databox *box) {
    flexInsertByType(&ml->fl[entry->nodeIdx], entry->fe, box);
    /* todo: rebalance if necessary */
}

void multilistMediumInsertByTypeAfter(multilistMedium *ml,
                                      const multilistEntry *entry,
                                      const databox *box) {
    flex **const fl = &ml->fl[entry->nodeIdx];
    flexEntry *next = flexNext(*fl, entry->fe);
    if (next == NULL) {
        flexPushByType(fl, box, FLEX_ENDPOINT_TAIL);
    } else {
        flexInsertByType(fl, next, box);
    }
    /* todo: rebalance if necessary */
}

/* ====================================================================
 * Delete with cursor
 * ==================================================================== */
void multilistMediumDelEntry(multilistIterator *iter, multilistEntry *entry) {
    multilistMedium *const ml = entry->ml;
    flexDelete(&ml->fl[entry->nodeIdx], &entry->fe);

    /* If deleted only element in F0 and F1 stil has data,
     * move F1 to F0 since F0 must always have elements (if data exists). */
    if (flexCount(F0) == 0 && flexCount(F1) > 0) {
        assert(entry->nodeIdx == 0);
        swapF();
    }

    if (iter->fe) {
        if (!iter->forward && entry->fe) {
            /* update for reverse direction */
            iter->fe = flexPrev(ml->fl[entry->nodeIdx], entry->fe);
        } else {
            /* update for forward direction */
            iter->fe = entry->fe;
        }
    }
}

/* ====================================================================
 * Delete by position
 * ==================================================================== */
bool multilistMediumDelRange(multilistMedium *ml, const mlOffsetId start,
                             const int64_t values) {
    const mlOffsetId countF0 = flexCount(F0);
    if (values < 0 || countF0 == 0) {
        return false;
    }

    const mlOffsetId countF1 = flexCount(F1);
    const mlOffsetId currentValues = countF0 + countF1;
    mlOffsetId extent = values; /* range is inclusive of start position */

    if (start >= 0 && extent > (currentValues - start)) {
        /* if requesting delete more elements than exist, limit to list size. */
        extent = currentValues - start;
    } else if (start < 0 && extent > (-start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -start;
    }

    assert(extent <= currentValues);

    /* get entry at index... */
    multilistEntry entry = {0};
    if (!multilistMediumIndex(ml, start, &entry)) {
        return false;
    }

    /* delete starting at index... */
    /* Cases:
     *      - delete entire list
     *      - delete all of F0, none of F1
     *      - delete none of F0, all of F1
     *      - delete all of F0, part of F1 [start > F0, end in F1]
     *      - delete part of F0, all of F1 [start in F0, end end of F1]
     *      - delete part of F0, part of F1 [start in F0, end in F1] */

    /* Note: we *already* verified 'extent' is <= currentValues above,
     * so if extent == currentValues, then we're deleting everything. */
    if (extent == currentValues) {
        /* If deleting from start to end including all values,
         * just clear both */
        flexReset(&F0);
        flexReset(&F1);
    } else if (entry.nodeIdx == 0 && extent == countF0) {
        /* If deleting all of F0, clear F0.
         * (which is the same as swapping F1 with an empty F0) */
        flexReset(&F0);
        swapF();
    } else if (entry.nodeIdx == 1 && extent == countF1) {
        /* If deleting all of F1, just clear F1. */
        flexReset(&F1);
    } else if (extent > countF0) {
        /* deleting more than F0, so clear F0, delete part of F1, then
         * swap F1 with F0 */
        flexReset(&F0);
        extent -= countF0;

        flexEntry *deleteStartF1 = flexIndex(F1, entry.offset);
        flexDeleteCount(&F1, &deleteStartF1, extent);
    } else if (entry.nodeIdx == 0) {
        /* deleting part of F0 and part of F1 */
        flexEntry *deleteStartF0 = flexIndex(F0, entry.offset);
        flexDeleteCount(&F0, &deleteStartF0, extent - countF1);

        extent -= (extent - countF1);
        assert(extent > 0);

        flexEntry *deleteStartF1 = flexHead(F1);
        flexDeleteCount(&F1, &deleteStartF1, extent);
    } else if (entry.nodeIdx == 1) {
        /* deleting none of F0 and part of F1 */
        flexEntry *deleteStartF1 = flexIndex(F1, entry.offset);
        flexDeleteCount(&F1, &deleteStartF1, extent);
    } else {
        assert(NULL && "unexpected combination of conditions!");
    }

    return true;
}

/* ====================================================================
 * Replace by index
 * ==================================================================== */
bool multilistMediumReplaceByTypeAtIndex(multilistMedium *ml, mlOffsetId index,
                                         const databox *box) {
    /* todo: need forward-back, cross-slot traversal */
    multilistEntry entry = {0};
    if (multilistMediumIndex(ml, index, &entry)) {
        flexReplaceByType(&ml->fl[entry.nodeIdx], entry.fe, box);
        return true;
    }

    return false;
}

/* ====================================================================
 * Iteration
 * ==================================================================== */
void multilistMediumIteratorInit(multilistMedium *ml, multilistIterator *iter,
                                 bool forward) {
    if (forward) {
        iter->offset = 0;
    } else {
        iter->offset = -1;
    }

    iter->nodeIdx = forward ? 0 : 1;
    iter->forward = forward;
    iter->ml = ml;
    iter->fe = flexIndexDirect(ml->fl[iter->nodeIdx], iter->offset);
}

bool multilistMediumIteratorInitAtIdx(multilistMedium *ml,
                                      multilistIterator *iter, mlOffsetId idx,
                                      bool forward) {
    multilistEntry entry = {0};

    if (multilistMediumIndex(ml, idx, &entry)) {
        multilistMediumIteratorInit(ml, iter, forward);
        iter->fe = flexIndexDirect(ml->fl[iter->nodeIdx], iter->offset);
        iter->offset = entry.offset;
        return true;
    }

    return false;
}

bool multilistMediumNext(multilistIterator *iter, multilistEntry *entry) {
    entry->ml = iter->ml;

    if (iter->fe) {
        /* Populate value from existing flex position */
        flexGetByType(iter->fe, &entry->box);

        entry->nodeIdx = iter->nodeIdx;

        entry->f = iter->f;
        entry->fe = iter->fe;
        entry->offset = iter->offset;

        /* use existing iterator offset and get prev/next as necessary. */
        if (iter->forward) {
            iter->fe = flexNext(iter->f, iter->fe);
            iter->offset += 1;
        } else {
            iter->fe = flexPrev(iter->f, iter->fe);
            iter->offset -= 1;
        }

        return true;
    }

    if (iter->forward) {
        /* Forward traversal */
        iter->nodeIdx++;
        iter->offset = 0;
    } else {
        /* Reverse traversal */
        iter->nodeIdx--;
        iter->offset = -1;
    }

    /* If we iterate to a too high or too low node, we are out of entries */
    if (iter->nodeIdx > 1 || iter->nodeIdx < 0) {
        return false;
    }

    iter->f = ((multilistMedium *)iter->ml)->fl[iter->nodeIdx];
    iter->fe = flexIndexDirect(iter->f, iter->offset);

    /* If we didn't get a next entry, no more entries exist anywhere. */
    if (!iter->fe) {
        return false;
    }

    return multilistMediumNext(iter, entry);
}

bool multilistMediumIndex(multilistMedium *ml, mlOffsetId index,
                          multilistEntry *entry) {
    entry->ml = ml;
    entry->offset = index;

    /* convert negative offset to positve offset */
    const bool reverse = index < 0 ? true : false; /* negative == reverse */
    if (reverse) {
        index = (-index) - 1;
    }

    /* if out of range of all elements, nothing to index */
    const mlOffsetId values = multilistMediumCount(ml);
    if (values > 0 && index >= values) {
        return false;
    }

    /* if index is beyond F0, jump over F0 into F1 */
    const mlOffsetId countF0 = flexCount(F0);
    mlNodeId useNode = 0;
    if (countF0 >= index) {
        index -= countF0;
        useNode = 1;
    }

    /* Record offset index used by this lookup */
    entry->offset = index;
    assert(entry->offset < (mlOffsetId)flexCount(ml->fl[useNode]));

    /* Get the actual element requested */
    entry->fe = flexIndex(ml->fl[useNode], index);
    if (!entry->fe) {
        entry->fe = flexHead(ml->fl[useNode]);
        return false;
    }

    flexGetByType(entry->fe, &entry->box);
    return true;
}

/* ====================================================================
 * Rotate
 * ==================================================================== */
void multilistMediumRotate(multilistMedium *ml) {
    const size_t countF0 = flexCount(F0);
    const size_t countF1 = flexCount(F1);

    /* If no elements OR if only one element, can't rotate. */
    if ((countF0 == 0 && countF1 == 0) || (countF0 + countF1) == 1) {
        return;
    }

    /* If F0 or F1 have just one element, swap their
     * positions so the ml head/tail exchange places. */
    if (countF0 == 1 || countF1 == 1) {
        swapF();
        return;
    }

    flex **tail = &ml->fl[TAIL_INDEX_FROM_COUNT(countF1)];

    /* Get tail entry position */
    flexEntry *fe = flexTail(*tail);

    /* Get tail entry */
    databox box = {{0}};
    flexGetByType(fe, &box);

    /* Copy tail entry to head (must happen before tail is deleted). */
    flexPushByType(&F0, &box, FLEX_ENDPOINT_HEAD);

    /* Remove tail entry. */
    flexDeleteRange(tail, -1, 1);
}

/* ====================================================================
 * Pop!
 * ==================================================================== */
bool multilistMediumPop(multilistMedium *ml, databox *box, bool fromTail) {
    const size_t countF0 = flexCount(F0);
    const size_t countF1 = flexCount(F1);

    /* If no data exists, we can't pop anything. */
    if (countF0 == 0 && countF1 == 0) {
        return false;
    }

    flex **ffl = &ml->fl[fromTail ? TAIL_INDEX_FROM_COUNT(countF1) : 0];
    /* if requested endpoint is empty, can't pop. */
    if (flexCount(*ffl) == 0) {
        return false;
    }

    const flexEndpoint flIdx =
        fromTail ? FLEX_ENDPOINT_TAIL : FLEX_ENDPOINT_HEAD;
    const flexEntry *fe = flexHeadOrTail(*ffl, flIdx);

    databox found = {{0}};
    if (fe) {
        flexGetByType(fe, &found);
        *box = found;
        databoxCopyBytesFromBox(box, &found);
        flexDeleteRange(ffl, flIdx, 1);
        return true;
    }

    return false;
}

/* ====================================================================
 * Insert by endpoint
 * ==================================================================== */
void multilistMediumPushByTypeHead(multilistMedium *ml, const databox *box) {
    flexPushByType(&F0, box, FLEX_ENDPOINT_HEAD);
    /* todo: rebalance */
}

void multilistMediumPushByTypeTail(multilistMedium *ml, const databox *box) {
    flexPushByType(&F1, box, FLEX_ENDPOINT_TAIL);
    /* todo: rebalance */
}

/* ====================================================================
 * Debuggles
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multilistMediumRepr(multilistMedium *ml) {
    flexRepr(F0);
    flexRepr(F1);
}
#endif
