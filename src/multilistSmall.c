#include "multilistSmall.h"
#include "multilistSmallInternal.h"

/* ====================================================================
 * Create
 * ==================================================================== */
/* A Small list has no header: the multilist's pointer is its flex, and
 * 'ml' here is a view the caller builds on the stack
 * (multilistSmallInternal.h) */
void multilistSmallInit(multilistSmall *ml) {
    /* @own: container; free=multilistSmallFree */
    ml->fl = flexNew();
}

/* Small keeps one flex and never compresses.  The fill limit and the
 * compress depth belong to the multilist that owns the tier (it keeps
 * them in its tagged pointer and decides when to move to Medium), so
 * the Small view has no field for either: the setters accept them for
 * signature parity with Full and ignore them. */
void multilistSmallSetCompressDepth(multilistSmall *, int) {
}

void multilistSmallSetFill(multilistSmall *, int) {
}

void multilistSmallSetOptions(multilistSmall *ml, int fill, int depth) {
    multilistSmallSetFill(ml, fill);
    multilistSmallSetCompressDepth(ml, depth);
}

/* ====================================================================
 * Free
 * ==================================================================== */
/* Frees the list's elements (the view itself is the caller's) */
void multilistSmallFree(multilistSmall *ml) {
    flexFree(ml->fl);
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

flex *multilistSmallDump(const multilistSmall *ml) {
    /* @own: caller; free=flexFree */
    return flexDuplicate(ml->fl);
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
    if (next == nullptr) {
        flexPushByTypeExact(&ml->fl, box, FLEX_ENDPOINT_TAIL);
    } else {
        flexInsertByType(&ml->fl, next, box);
    }
}

/* ====================================================================
 * Delete with cursor
 * ==================================================================== */
/* The iterator holds the list's flex in iter->f; the delete may move it
 * there, and the caller (multilist.c) stores it back in the list's
 * pointer */
void multilistSmallDelEntry(multilistIterator *iter, multilistEntry *entry) {
    flexDelete(&iter->f, &entry->fe);

    if (iter->fe) {
        if (!iter->forward && entry->fe) {
            /* update for reverse direction */
            iter->fe = flexPrev(iter->f, entry->fe);
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
    /* The flex, not 'ml': the view lives on the caller's stack.
     * multilist.c sets iter->ml (the list's handle, for a writable
     * iterator) */
    iter->f = ml->fl;
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
void multilistSmallReleaseIterator(multilistIterator *) {
}

bool multilistSmallNext(multilistIterator *iter, multilistEntry *entry) {
    flex *const fl = iter->f;
    entry->ml = fl;
    entry->nodeIdx = iter->nodeIdx;

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
    entry->ml = ml->fl; /* the flex: 'ml' is a view on the caller's stack */

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
    flexGetByTypeInto(entry->fe, &entry->box, nullptr);
    return true;
}

bool multilistSmallIndex(multilistSmall *ml, mlOffsetId index,
                         multilistEntry *entry) {
    return multilistSmallIndexNear(ml, index, nullptr, 0, entry);
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
        flexGetByTypeDetached(fe, box);
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
