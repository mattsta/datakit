#pragma once

#include "multilistMedium.h"
#include "multilistSmall.h"

/* This is a sad wrapper header because many multilistFull() functions
 * take an explicit compression state parameter, but Small/Medium don't,
 * but each function call must have the same params for our pointer-tagging
 * based type switching to work... so create mock function calls and throw
 * away the parameter we know we won't use. */

/* ====================================================================
 * Small to Full Prototype Conforming Adapter
 * ==================================================================== */
/* Create */
#define multilistSmallAdapterCreate() multilistSmallCreate()
#define multilistSmallAdapterNew(fill, compress)                               \
    multilistSmallNew(fill, compress)

/* Free */
#define multilistSmallAdapterFree(ml) multilistSmallFree(ml)

/* Metadata */
#define multilistSmallAdapterCount(ml) multilistSmallCount(ml)
#define multilistSmallAdapterBytes(ml) multilistSmallBytes(ml)

/* Settings */
#define multilistSmallAdapterSetCompressDepth(ml, depth)                       \
    multilistSmallSetCompressDepth(ml, depth)
#define multilistSmallAdapterSetFill(ml, fill) multilistSmallSetFill(ml, fill)
#define multilistSmallAdapterSetOptions(ml, fill, depth)                       \
    multilistSmallSetOptions(ml, fill, depth)

/* Insert */
#define multilistSmallAdapterPushByTypeHead(ml, state, box)                    \
    multilistSmallPushByTypeHead(ml, box)
#define multilistSmallAdapterPushByTypeTail(ml, state, box)                    \
    multilistSmallPushByTypeTail(ml, box)

/* Bulk appending not conforming to multilist* protocol */
#define multilistSmallAdapterAppendFlex(ml, fl) multilistSmallAppendFlex(ml, fl)
#define multilistSmallAdapterAppendValuesFromFlex(ml, fl)                      \
    multilistSmallaluesFromFlex(ml, fl)
#define multilistSmallAdapterNewFromFlex(fl) multilistSmallNewFromFlex(fl)
#define multilistSmallAdapterNewFromFlexConsume(fl)                            \
    multilistSmallNewFromFlexConsume(fl)

/* Insert with entry cursor */
#define multilistSmallAdapterInsertByTypeBefore(ml, state, entry, box)         \
    multilistSmallInsertByTypeBefore(ml, entry, box)
#define multilistSmallAdapterInsertByTypeAfter(ml, state, entry, box)          \
    multilistSmallInsertByTypeAfter(ml, entry, box)

/* Delete based on entry cursor */
#define multilistSmallAdapterDelEntry(m, iter, entry)                          \
    multilistSmallDelEntry(m, iter, entry)

/* Delete based on position */
#define multilistSmallAdapterDelRange(ml, state, start, values)                \
    multilistSmallDelRange(ml, start, values)

/* Replace based on index */
#define multilistSmallAdapterReplaceByTypeAtIndex(ml, state, index, box)       \
    multilistSmallReplaceByTypeAtIndex(ml, index, box)

/* Iterator generation */
#define multilistSmallAdapterIteratorInit(ml, state, iter, forward, readOnly)  \
    multilistSmallIteratorInit(ml, iter, forward)
#define multilistSmallAdapterIteratorInitAtIdx(ml, state, iter, idx, forward,  \
                                               readOnly)                       \
    multilistSmallIteratorInitAtIdx(ml, iter, idx, forward)

/* Iterating */
#define multilistSmallAdapterNext(iter, entry) multilistSmallNext(iter, entry)

/* Reset iteration positions */
#define multilistSmallAdapterRewind(ml, iter) multilistSmallRewind(ml, iter)
#define multilistSmallAdapterRewindTail(ml, iter)                              \
    multilistSmallRewindTail(ml, iter)

/* Close iterator */
#define multilistSmallAdapterReleaseIterator(iter)                             \
    multilistSmallReleaseIterator(iter)

/* Copy entire */
#define multilistSmallAdapterDuplicate(orig) multilistSmallDuplicate(orig)

/* Get entry based on index */
#define multilistSmallAdapterIndex(ml, state, index, entry, open)              \
    multilistSmallIndex(ml, index, entry)

/* Move ends of list */
#define multilistSmallAdapterRotate(ml, state) multilistSmallRotate(ml)

/* Remove and return head or tail of list */
#define multilistSmallAdapterPop(ml, state, box, fromTail)                     \
    multilistSmallPop(ml, box, fromTail)
#define multilistSmallAdapterPopFromTail(ml, box)                              \
    multilistSmallPop(ml, box, true)
#define multilistSmallAdapterPopFromHead(ml, box)                              \
    multilistSmallPop(ml, box, false)

/* ====================================================================
 * Medium to Full Prototype Conforming Adapter
 * ==================================================================== */

/* Create */
#define multilistMediumAdapterCreate() multilistMediumCreate()
#define multilistMediumAdapterNew(fill, compress)                              \
    multilistMediumNew(fill, compress)

/* Free */
#define multilistMediumAdapterFree(ml) multilistMediumFree(ml)

/* Metadata */
#define multilistMediumAdapterCount(ml) multilistMediumCount(ml)
#define multilistMediumAdapterBytes(ml) multilistMediumBytes(ml)

/* Settings */
#define multilistMediumAdapterSetCompressDepth(ml, depth)                      \
    multilistMediumSetCompressDepth(ml, depth)
#define multilistMediumAdapterSetFill(ml, fill) multilistMediumSetFill(ml, fill)
#define multilistMediumAdapterSetOptions(ml, fill, depth)                      \
    multilistMediumSetOptions(ml, fill, depth)

/* Insert */
#define multilistMediumAdapterPushByTypeHead(ml, state, box)                   \
    multilistMediumPushByTypeHead(ml, box)
#define multilistMediumAdapterPushByTypeTail(ml, state, box)                   \
    multilistMediumPushByTypeTail(ml, box)

/* Bulk appending not conforming to multilist* protocol */
#define multilistMediumAdapterAppendFlex(ml, fl)                               \
    multilistMediumAppendFlex(ml, fl)
#define multilistMediumAdapterAppendValuesFromFlex(ml, fl)                     \
    multilistMediumAppendValuesFromFlex(ml, fl)
#define multilistMediumAdapterNewFromFlex(fl) multilistMediumNewFromFlex(fl)
#define multilistMediumAdapterNewFromFlexConsume(fl)                           \
    multilistMediumNewFromFlexConsume(fl)
#define multilistMediumAdapterNewFromFlexConsumeGrow(_ml, fl)                  \
    multilistMediumNewFromFlexConsumeGrow(_ml, fl)

/* Insert with entry cursor */
#define multilistMediumAdapterInsertByTypeBefore(ml, state, entry, box)        \
    multilistMediumInsertByTypeBefore(ml, entry, box)
#define multilistMediumAdapterInsertByTypeAfter(ml, state, entry, box)         \
    multilistMediumInsertByTypeAfter(ml, entry, box)

/* Delete based on entry cursor */
#define multilistMediumAdapterDelEntry(m, iter, entry)                         \
    multilistMediumDelEntry(m, iter, entry)

/* Delete based on position */
#define multilistMediumAdapterDelRange(ml, state, start, values)               \
    multilistMediumDelRange(ml, start, values)

/* Replace based on index */
#define multilistMediumAdapterReplaceByTypeAtIndex(ml, state, index, box)      \
    multilistMediumReplaceByTypeAtIndex(ml, index, box)

/* Iterator generation */
#define multilistMediumAdapterIteratorInit(ml, state, iter, forward, readOnly) \
    multilistMediumIteratorInit(ml, iter, forward)
#define multilistMediumAdapterIteratorInitAtIdx(ml, state, iter, idx, forward, \
                                                readOnly)                      \
    multilistMediumIteratorInitAtIdx(ml, iter, idx, forward)

/* Iterating */
#define multilistMediumAdapterNext(iter, entry) multilistMediumNext(iter, entry)

/* Reset iteration positions */
#define multilistMediumAdapterRewind(ml, iter) multilistMediumRewind(ml, iter)
#define multilistMediumAdapterRewindTail(ml, iter)                             \
    multilistMediumRewindTail(ml, iter)

/* Close iterator */
#define multilistMediumAdapterReleaseIterator(iter)                            \
    multilistMediumReleaseIterator(iter)

/* Copy entire */
#define multilistMediumAdapterDuplicate(orig) multilistMediumDuplicate(orig)

/* Get entry based on index */
#define multilistMediumAdapterIndex(ml, state, index, entry, open)             \
    multilistMediumIndex(ml, index, entry)

/* Move ends of list */
#define multilistMediumAdapterRotate(ml, state) multilistMediumRotate(ml)

/* Remove and return head or tail of list */
#define multilistMediumAdapterPop(ml, state, box, fromTail)                    \
    multilistMediumPop(ml, box, fromTail)
#define multilistMediumAdapterPopFromTail(ml, box)                             \
    multilistMediumPop(ml, box, true)
#define multilistMediumAdapterPopFromHead(ml, box)                             \
    multilistMediumPop(ml, box, false)
