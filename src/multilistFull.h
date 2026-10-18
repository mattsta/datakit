/* multilistFull.h - large space-efficient growable list interface
 *
 * Copyright 2016 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "datakit.h"

#include "mflex.h"
#include "multiarray.h"
#include "multilistCommon.h"

typedef struct multilistFull multilistFull;

/* Create */
multilistFull *multilistFullCreate(void);
multilistFull *multilistFullNew(uint32_t fill, uint32_t compress);

/* Metadata */
size_t multilistFullCount(const multilistFull *ml);
size_t multilistFullBytes(const multilistFull *ml);
/* Physical bytes (compressed nodes measured at stored size) */
size_t multilistFullBytesActual(const multilistFull *ml);

/* Settings */
/* Set the compress depth (clamped to 32767) and put every existing node
 * on the depth rule for it: the 'depth' nodes at each end are never
 * compressed, and every node between them is compressed; depth 0
 * decompresses every node.  The list holds no mflexState, so when a node
 * has to be compressed or decompressed this borrows a temporary one for
 * the call; when nodes only need retagging, or the depth is unchanged,
 * nothing is allocated.  A caller that holds a state can pass it to
 * multilistFullSetCompressDepthWithState instead. */
void multilistFullSetCompressDepth(multilistFull *ml, uint32_t depth);
void multilistFullSetCompressDepthWithState(multilistFull *ml,
                                            mflexState *state, uint32_t depth);
void multilistFullSetFill(multilistFull *ml, uint32_t fill);
/* SetFill then SetCompressDepth; SetOptions and SetDepth are the same
 * call under two names. */
void multilistFullSetOptions(multilistFull *ml, uint32_t fill, uint32_t depth);
void multilistFullSetDepth(multilistFull *ml, uint32_t fill, uint32_t depth);

/* Free */
void multilistFullFree(multilistFull *ml);

/* Insert */
void multilistFullPushByTypeHead(multilistFull *ml, mflexState *state,
                                 const databox *box);
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box);

/* Bulk appending */
/* AppendFlex keeps 'fl' inside a new tail node and keeps the depth rule:
 * the node the new tail pushes into the interior is compressed, through a
 * temporary mflexState that is allocated only when that is needed.
 * AppendFlexWithState uses the caller's state instead.  Both take
 * ownership of 'fl': an empty 'fl' is freed and adds nothing, and on an
 * empty list 'fl' becomes the list's only node. */
void multilistFullAppendFlex(multilistFull *ml, flex *fl);
void multilistFullAppendFlexWithState(multilistFull *ml, mflexState *state,
                                      flex *fl);
/* Push every element of 'fl' (not consumed) onto the tail, one by one;
 * returns 'ml'.  AppendValuesFlex is the same call under another name. */
multilistFull *multilistFullAppendValuesFromFlex(multilistFull *ml,
                                                 mflexState *state,
                                                 const flex *fl);
multilistFull *multilistFullAppendValuesFlex(multilistFull *ml,
                                             mflexState *state, const flex *fl);
/* A new list (multilistFullNew(fill, compress)) holding every element of
 * 'fl' (not consumed), pushed one by one.  multilistFulllCreateFlex is
 * the same call under another name. */
multilistFull *multilistFullNewFromFlex(int fill, int compress,
                                        mflexState *state, const flex *fl);
multilistFull *multilistFulllCreateFlex(int fill, int compress,
                                        mflexState *state, const flex *fl);
/* Build a list from the 'flCount' flexes in 'fl' (any count, 0 included),
 * in order, taking ownership of every one of them.  '_ml' is NULL or an
 * allocation to reuse, and is reallocated into the list.  A flex of two
 * or more elements becomes two nodes (split at its middle), a
 * one-element flex one node, and an empty flex is freed.  The depth rule
 * is applied to every node through 'state'. */
multilistFull *multilistFullNewFromFlexConsumeGrow(void *_ml, mflexState *state,
                                                   flex *fl[], size_t flCount,
                                                   uint32_t depth,
                                                   uint32_t limit);

/* Insert with entry cursor */
void multilistFullInsertByTypeBefore(multilistFull *ml, mflexState *state[2],
                                     const multilistEntry *entry,
                                     const databox *box);
void multilistFullInsertByTypeAfter(multilistFull *ml, mflexState *state[2],
                                    const multilistEntry *entry,
                                    const databox *box);

/* Delete based on entry cursor */
void multilistFullDelEntry(multilistIterator *iter, multilistEntry *entry);

/* Delete based on position */
bool multilistFullDelRange(multilistFull *ml, mflexState *state,
                           const mlOffsetId start, const int64_t values);

/* Replace based on index */
bool multilistFullReplaceByTypeAtIndex(multilistFull *ml, mflexState *state,
                                       mlOffsetId index, const databox *box);

/* Iterator generation */
void multilistFullIteratorInit(multilistFull *ml, mflexState *state[2],
                               multilistIterator *iter, bool forward,
                               bool readOnly);
#define multilistFullIteratorInitReadOnly(ml, s, iter, forward)                \
    multilistFullIteratorInit(ml, s, iter, forward, true)
#define multilistFullIteratorInitForwardReadOnly(ml, s, iter)                  \
    multilistFullIteratorInit(ml, s, iter, true, true)
#define multilistFullIteratorInitForward(ml, s, iter)                          \
    multilistFullIteratorInit(ml, s, iter, true, false)
#define multilistFullIteratorInitReverse(ml, s, iter)                          \
    multilistFullIteratorInit(ml, s, iter, false, false)
#define multilistFullIteratorInitReverseReadOnly(ml, s, iter)                  \
    multilistFullIteratorInit(ml, s, iter, false, true)

bool multilistFullIteratorInitAtIdx(multilistFull *ml, mflexState *state[2],
                                    multilistIterator *iter, mlOffsetId idx,
                                    bool forward, bool readOnly);
#define multilistFullIteratorInitAtIdxForward(ml, s, iter, idx)                \
    multilistFullIteratorInitAtIdx(ml, s, iter, idx, true)
#define multilistFullIteratorInitAtIdxForwardReadOnly(ml, s, iter, idx)        \
    multilistFullIteratorInitAtIdx(ml, s, iter, idx, true, true)
#define multilistFullIteratorInitAtIdxReverse(ml, s, iter, idx)                \
    multilistFullIteratorInitAtIdx(ml, s, iter, idx, false)
#define multilistFullIteratorInitAtIdxReverseReadOnly(ml, s, iter, idx)        \
    multilistFullIteratorInitAtIdx(ml, s, iter, idx, false, true)

/* Iterating */
bool multilistFullNext(multilistIterator *iter, multilistEntry *entry);

/* Reset iteration positions */
void multilistFullRewind(multilistFull *ml, multilistIterator *iter);
void multilistFullRewindTail(multilistFull *ml, multilistIterator *iter);

/* Close iterator */
void multilistFullIteratorRelease(multilistIterator *iter);

/* Copy entire multilistFull */
multilistFull *multilistFullDuplicate(multilistFull *orig);

/* Every element, in order, as one new flex (see multilistDump) */
flex *multilistFullDump(const multilistFull *ml, mflexState *state);

/* Get entry based on index */
bool multilistFullIndex(multilistFull *ml, mflexState *state, mlOffsetId index,
                        multilistEntry *entry, bool openNode);
#define multilistFullIndexGet(ml, s, i, e) multilistFullIndex(ml, s, i, e, true)
#define multilistFullIndexCheck(ml, s, i, e)                                   \
    multilistFullIndex(ml, s, i, e, false)

/* Index starting from a caller-held anchor entry (with its known global
 * index) when that is closer than either endpoint; node counts are
 * skipped whole from the anchor in either direction.  The anchor must
 * have been obtained after the most recent mutation. */
bool multilistFullIndexNear(multilistFull *ml, mflexState *state,
                            mlOffsetId index, const multilistEntry *anchor,
                            mlOffsetId anchorIdx, multilistEntry *entry,
                            bool openNode);

/* Move ends of list */
void multilistFullRotate(multilistFull *ml, mflexState *state[2]);

/* Remove and return head or tail of list as an owned value; 'box' must be
 * zeroed or DATABOX_BIG_INIT'd (see multilistPop in multilist.h) */
bool multilistFullPop(multilistFull *ml, mflexState *state, databox *box,
                      bool fromTail);
#define multilistFullPopTail(ml, s, box) multilistFullPop(ml, s, box, true)
#define multilistFullPopHead(ml, s, box) multilistFullPop(ml, s, box, false)

#ifdef DATAKIT_TEST
void multilistFullRepr(multilistFull *ml);
int multilistFullTest(int argc, char *argv[]);
#endif
