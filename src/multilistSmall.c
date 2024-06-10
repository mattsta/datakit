#include "multilistSmall.h"
#include "multilistSmallInternal.h"

/* ====================================================================
 * Create
 * ==================================================================== */
static multilistSmall *multilistSmallCreateContainer(void) {
    multilistSmall *ml = zcalloc(1, sizeof(*ml));
    return ml;
}

multilistSmall *multilistSmallCreate(void) {
    multilistSmall *ml = multilistSmallCreateContainer();
    ml->fl = flexNew();
    return ml;
}

/* ====================================================================
 * Copy
 * ==================================================================== */
multilistSmall *multilistSmallDuplicate(multilistSmall *orig) {
    multilistSmall *ml = multilistSmallCreate();
    ml->fl = flexDuplicate(orig->fl);
    return ml;
}

/* ====================================================================
 * Free
 * ==================================================================== */
void multilistSmallFree(multilistSmall *ml) {
    if (ml) {
        flexFree(ml->fl);
        zfree(ml);
    }
}

/* ====================================================================
 * Metadata
 * ==================================================================== */
size_t multilistSmallCount(const multilistSmall *ml) {
    return flexCount(ml->fl);
}

size_t multilistSmallBytes(const multilistSmall *ml) {
    return flexBytes(ml->fl);
}

/* ====================================================================
 * Bulk Operations
 * ==================================================================== */
void multilistSmallAppendFlex(multilistSmall *ml, const flex *fl) {
    flexBulkAppendFlex(&ml->fl, fl);
}

void multilistSmallAppendValuesFromFlex(multilistSmall *ml, const flex *fl) {
    multilistSmallAppendFlex(ml, fl);
}

multilistSmall *multilistSmallNewFromFlexConsume(flex *fl) {
    multilistSmall *ml = multilistSmallCreateContainer();
    ml->fl = fl;
    return ml;
}

multilistSmall *multilistSmallNewFromFlex(const flex *fl) {
    multilistSmall *ml = multilistSmallCreateContainer();
    ml->fl = flexDuplicate(fl);
    return ml;
}

/* ====================================================================
 * Insert with cursor
 * ==================================================================== */
void multilistSmallInsertByTypeBefore(multilistSmall *ml,
                                      const multilistEntry *entry,
                                      const databox *box) {
    flexInsertByType(&ml->fl, entry->fe, box);
}

void multilistSmallInsertByTypeAfter(multilistSmall *ml,
                                     const multilistEntry *entry,
                                     const databox *box) {
    flexEntry *next = flexNext(ml->fl, entry->fe);
    if (next == NULL) {
        flexPushByType(&ml->fl, box, FLEX_ENDPOINT_TAIL);
    } else {
        flexInsertByType(&ml->fl, next, box);
    }
}

/* ====================================================================
 * Delete with cursor
 * ==================================================================== */
void multilistSmallDelEntry(multilistIterator *iter, multilistEntry *entry) {
    flexDelete(&((multilistSmall *)iter->ml)->fl, &entry->fe);

    if (iter->fe) {
        if (!iter->forward && entry->fe) {
            /* update for reverse direction */
            iter->fe = flexPrev(((multilistSmall *)iter->ml)->fl, entry->fe);
        } else {
            /* update for forward direction */
            iter->fe = entry->fe;
        }
    }
}

/* ====================================================================
 * Delete by position
 * ==================================================================== */
bool multilistSmallDelRange(multilistSmall *ml, const mlOffsetId start,
                            const int64_t values) {
    const mlOffsetId currentValues = flexCount(ml->fl);
    if (values < 0 || currentValues == 0) {
        return false;
    }

    mlOffsetId extent = values;
    if (start >= 0 && extent > (currentValues - start)) {
        /* if requesting delete more elements than exist, limit to list size. */
        extent = currentValues - start;
    } else if (start < 0 && extent > (-start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -start;
    }

    flexDeleteRange(&ml->fl, start, extent);
    return true;
}

/* ====================================================================
 * Replace by index
 * ==================================================================== */
bool multilistSmallReplaceByTypeAtIndex(multilistSmall *ml, mlOffsetId index,
                                        const databox *box) {
    flexEntry *fe = flexIndex(ml->fl, index);
    flexReplaceByType(&ml->fl, fe, box);
    return true;
}

/* ====================================================================
 * Iteration
 * ==================================================================== */
void multilistSmallIteratorInit(multilistSmall *ml, multilistIterator *iter,
                                bool forward) {
    if (forward) {
        iter->offset = 0;
    } else {
        iter->offset = -1;
    }

    iter->forward = forward;
    iter->ml = ml;
    iter->fe = flexIndexDirect(ml->fl, iter->offset);
}

bool multilistSmallIteratorInitAtIdx(multilistSmall *ml,
                                     multilistIterator *iter, mlOffsetId idx,
                                     bool forward) {
    multilistEntry entry = {0};

    if (multilistSmallIndex(ml, idx, &entry)) {
        multilistSmallIteratorInit(ml, iter, forward);
        iter->offset = entry.offset;
        iter->fe = flexIndexDirect(ml->fl, iter->offset);
        return true;
    }

    return false;
}

bool multilistSmallNext(multilistIterator *iter, multilistEntry *entry) {
    entry->ml = iter->ml;
    entry->nodeIdx = iter->nodeIdx;
    flex *const fl = ((multilistSmall *)iter->ml)->fl;

    if (iter->fe) {
        /* Populate value from existing flex position */
        flexGetByType(iter->fe, &entry->box);

        entry->fe = iter->fe;
        entry->offset = iter->offset;

        /* use existing iterator offset and get prev/next as necessary. */
        if (iter->forward) {
            iter->fe = flexNext(fl, iter->fe);
            iter->offset += 1;
        } else {
            iter->fe = flexPrev(fl, iter->fe);
            iter->offset -= 1;
        }

        return true;
    }

    return false;
}

bool multilistSmallIndex(multilistSmall *ml, mlOffsetId index,
                         multilistEntry *entry) {
    entry->ml = ml;
    entry->fe = flexIndex(ml->fl, index);
    entry->offset = index;

    if (!entry->fe) {
        /* Used for requesting the 0 index in an empty list */
        entry->fe = flexHead(ml->fl);
        return false;
    }

    flexGetByType(entry->fe, &entry->box);
    return true;
}

/* ====================================================================
 * Rotate
 * ==================================================================== */
void multilistSmallRotate(multilistSmall *ml) {
    if (flexCount(ml->fl) <= 1) {
        return;
    }

    /* Get tail entry position */
    flexEntry *fe = flexTail(ml->fl);

    /* Get tail entry */
    databox box = {{0}};
    flexGetByType(fe, &box);

    /* Copy tail entry to head (must happen before tail is deleted). */
    flexPushByType(&ml->fl, &box, FLEX_ENDPOINT_HEAD);

    /* Remove tail entry. */
    flexDeleteRange(&ml->fl, -1, 1);
}

/* ====================================================================
 * Pop!
 * ==================================================================== */
bool multilistSmallPop(multilistSmall *ml, databox *box, bool fromTail) {
    if (flexCount(ml->fl) == 0) {
        return false;
    }

    const flexEndpoint flIdx =
        fromTail ? FLEX_ENDPOINT_TAIL : FLEX_ENDPOINT_HEAD;
    const flexEntry *fe = flexHeadOrTail(ml->fl, flIdx);

    databox found = {{0}};
    if (fe) {
        flexGetByType(fe, &found);
        *box = found;
        databoxCopyBytesFromBox(box, &found);
        flexDeleteRange(&ml->fl, flIdx, 1);
        return true;
    }

    return false;
}

/* ====================================================================
 * Insert by endpoint
 * ==================================================================== */
void multilistSmallPushByTypeHead(multilistSmall *ml, const databox *box) {
    flexPushByType(&ml->fl, box, FLEX_ENDPOINT_HEAD);
}

void multilistSmallPushByTypeTail(multilistSmall *ml, const databox *box) {
    flexPushByType(&ml->fl, box, FLEX_ENDPOINT_TAIL);
}

/* ====================================================================
 * Debuggles
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multilistSmallRepr(multilistSmall *ml) {
    flexRepr(ml->fl);
}
#endif
