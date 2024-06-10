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

/* Settings */
void multilistFullSetCompressDepth(multilistFull *ml, uint32_t depth);
void multilistFullSetFill(multilistFull *ml, uint32_t fill);
void multilistFullSetOptions(multilistFull *ml, uint32_t fill, uint32_t depth);

/* Free */
void multilistFullFree(multilistFull *ml);

/* Insert */
void multilistFullPushByTypeHead(multilistFull *ml, mflexState *state,
                                 const databox *box);
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box);

/* Bulk appending */
/* AppendFlex keeps 'fl' inside new tail. */
void multilistFullAppendFlex(multilistFull *ml, flex *fl);
multilistFull *multilistFullAppendValuesFlex(multilistFull *ml,
                                             mflexState *state, const flex *fl);
multilistFull *multilistFulllCreateFlex(int fill, int compress,
                                        mflexState *state, const flex *fl);
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

/* Get entry based on index */
bool multilistFullIndex(multilistFull *ml, mflexState *state, mlOffsetId index,
                        multilistEntry *entry, bool openNode);
#define multilistFullIndexGet(ml, s, i, e) multilistFullIndex(ml, s, i, e, true)
#define multilistFullIndexCheck(ml, s, i, e)                                   \
    multilistFullIndex(ml, s, i, e, false)

/* Move ends of list */
void multilistFullRotate(multilistFull *ml, mflexState *state[2]);

/* Remove and return head or tail of list */
bool multilistFullPop(multilistFull *ml, mflexState *state, databox *box,
                      bool fromTail);
#define multilistFullPopTail(ml, s, box) multilistFullPop(ml, s, box, true)
#define multilistFullPopHead(ml, s, box) multilistFullPop(ml, s, box, false)

#ifdef DATAKIT_TEST
void multilistFullRepr(multilistFull *ml);
int multilistFullTest(int argc, char *argv[]);
#endif
