#include "multilistSmall.h"
#include "multilistSmallInternal.h"

/* ====================================================================
 * Create
 * ==================================================================== */
static multilistSmall *multilistSmallCreateContainer(void) {
    /* @own: caller; free=multilistSmallFree */
    multilistSmall *ml = zcalloc(1, sizeof(*ml));
    return ml;
}

multilistSmall *multilistSmallCreate(void) {
    multilistSmall *ml = multilistSmallCreateContainer();
    /* @own: container; free=flexFree */
    ml->fl = flexNew();
    return ml;
}

/* Small keeps one flex and never compresses.  The fill limit and the
 * compress depth belong to the multilist that owns the tier (it keeps
 * them in its tagged pointer and decides when to move to Medium), so
 * the Small struct has no field for either: they are accepted for
 * signature parity with Full and ignored. */
multilistSmall *multilistSmallNew(int fill, int compress) {
    (void)fill;
    (void)compress;
    return multilistSmallCreate();
}

void multilistSmallSetCompressDepth(multilistSmall *ml, int depth) {
    (void)ml;
    (void)depth;
}

void multilistSmallSetFill(multilistSmall *ml, int fill) {
    (void)ml;
    (void)fill;
}

void multilistSmallSetOptions(multilistSmall *ml, int fill, int depth) {
    multilistSmallSetFill(ml, fill);
    multilistSmallSetCompressDepth(ml, depth);
}

/* ====================================================================
 * Copy
 * ==================================================================== */
multilistSmall *multilistSmallDuplicate(multilistSmall *orig) {
    multilistSmall *ml = multilistSmallCreate();
    /* Create() pre-allocated an empty flex; release before overwrite */
    flexFree(ml->fl);
    /* @own: container; free=flexFree */
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

flex *multilistSmallDump(const multilistSmall *ml) {
    /* @own: caller; free=flexFree */
    return flexDuplicate(ml->fl);
}

multilistSmall *multilistSmallNewFromFlex(const flex *fl) {
    multilistSmall *ml = multilistSmallCreateContainer();
    /* @own: container; free=flexFree */
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
        flexPushByTypeExact(&ml->fl, box, FLEX_ENDPOINT_TAIL);
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

    multilistIterOffsetAfterDelete_(iter, entry->offset);
}

/* ====================================================================
 * Delete by position
 * ==================================================================== */
/* Delete up to 'values' elements starting at 'start' (negative counts
 * from the tail).  Returns true if anything was deleted; a count of zero
 * or less, an empty list, or a start outside the list deletes nothing
 * and returns false. */
bool multilistSmallDelRange(multilistSmall *ml, const mlOffsetId start,
                            const int64_t values) {
    const mlOffsetId currentValues = flexCount(ml->fl);
    if (values <= 0 || start >= currentValues || start < -currentValues) {
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
/* Returns false, changing nothing, when 'index' is outside the list. */
bool multilistSmallReplaceByTypeAtIndex(multilistSmall *ml, mlOffsetId index,
                                        const databox *box) {
    const mlOffsetId values = flexCount(ml->fl);
    if (index >= values || index < -values) {
        return false;
    }

    flexEntry *fe = flexIndex(ml->fl, (int32_t)index);
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
    multilistEntry entry = {};

    if (multilistSmallIndex(ml, idx, &entry)) {
        multilistSmallIteratorInit(ml, iter, forward);
        iter->offset = entry.offset;
        iter->fe = flexIndexDirect(ml->fl, iter->offset);
        return true;
    }

    return false;
}

/* Restart an initialized iterator at the head (forward) or the tail
 * (reverse).  Small holds no per-iterator state to release, so this is a
 * fresh init; it is safe mid-iteration. */
void multilistSmallRewind(multilistSmall *ml, multilistIterator *iter) {
    multilistSmallIteratorInit(ml, iter, true);
}

void multilistSmallRewindTail(multilistSmall *ml, multilistIterator *iter) {
    multilistSmallIteratorInit(ml, iter, false);
}

/* Release an iterator.  The Full tier closes the node a writable
 * iterator holds open; a Small iterator reads the live flex directly and
 * holds nothing open, so there is nothing to close and nothing changes. */
void multilistSmallReleaseIterator(multilistIterator *iter) {
    (void)iter;
}

bool multilistSmallNext(multilistIterator *iter, multilistEntry *entry) {
    entry->ml = iter->ml;
    entry->nodeIdx = iter->nodeIdx;
    flex *const fl = ((multilistSmall *)iter->ml)->fl;

    if (iter->fe) {
        /* Populate value from existing flex position; a wide value
         * lands in the iterator's slot */
        flexGetByTypeInto(iter->fe, &entry->box, &iter->wide);

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

/* The one positional-lookup engine for the Small tier: with a
 * caller-held anchor (a previously located entry plus its known global
 * index) the flex walk starts from the nearest of head / anchor /
 * tail; without one it is the plain two-way endpoint walk. */
bool multilistSmallIndexNear(multilistSmall *ml, mlOffsetId index,
                             const multilistEntry *anchor, mlOffsetId anchorIdx,
                             multilistEntry *entry) {
    entry->ml = ml;

    /* Check the range at full width: the flex walk takes an int32_t, so
     * an index beyond it would otherwise wrap onto a real element. */
    const mlOffsetId values = flexCount(ml->fl);
    if (index >= values || index < -values) {
        /* Aimed at the head, so an empty list can take a first insert */
        entry->offset = 0;
        entry->fe = flexHead(ml->fl);
        return false;
    }

    entry->offset = (int32_t)index;
    const mlOffsetId a = anchorIdx < 0 ? values + anchorIdx : anchorIdx;

    entry->fe = (anchor && anchor->fe)
                    ? flexIndexFromAnchor(ml->fl, (int32_t)index, anchor->fe,
                                          (int32_t)a)
                    : flexIndex(ml->fl, index);

    if (!entry->fe) {
        /* Used for requesting the 0 index in an empty list */
        entry->fe = flexHead(ml->fl);
        return false;
    }

    /* A single read has no wide storage: a wide value is deferred */
    flexGetByTypeInto(entry->fe, &entry->box, NULL);
    return true;
}

bool multilistSmallIndex(multilistSmall *ml, mlOffsetId index,
                         multilistEntry *entry) {
    return multilistSmallIndexNear(ml, index, NULL, 0, entry);
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

    /* Get tail entry (a 96/128-bit value lands in 'wide') */
    databox box = {};
    databoxWide wide;
    flexGetByTypeInto(fe, &box, &wide);

    /* Copy tail entry to head (must happen before tail is deleted). */
    flexPushByTypeExact(&ml->fl, &box, FLEX_ENDPOINT_HEAD);

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

    if (fe) {
        /* The element is about to be deleted, so hand out an owned copy. */
        multilistPopCopy_(fe, box);
        flexDeleteRange(&ml->fl, flIdx, 1);
        return true;
    }

    return false;
}

/* ====================================================================
 * Insert by endpoint
 * ==================================================================== */
void multilistSmallPushByTypeHead(multilistSmall *ml, const databox *box) {
    flexPushByTypeExact(&ml->fl, box, FLEX_ENDPOINT_HEAD);
}

void multilistSmallPushByTypeTail(multilistSmall *ml, const databox *box) {
    flexPushByTypeExact(&ml->fl, box, FLEX_ENDPOINT_TAIL);
}

/* ====================================================================
 * Debuggles
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multilistSmallRepr(multilistSmall *ml) {
    flexRepr(ml->fl);
}
#endif
