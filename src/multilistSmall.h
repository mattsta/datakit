/* multilistSmall.h - small growable list interface
 *
 * Copyright 2014-2016 Matt Stancliff <matt@genges.com>
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

#include "flex.h"

#include "multilistCommon.h"

typedef struct multilistSmall multilistSmall;

/* Create */
multilistSmall *multilistSmallCreate(void);
multilistSmall *multilistSmallNew(int fill, int compress);

/* Free */
void multilistSmallFree(multilistSmall *ml);

/* Metadata */
size_t multilistSmallCount(const multilistSmall *ml);
size_t multilistSmallBytes(const multilistSmall *ml);

/* Settings */
void multilistSmallSetCompressDepth(multilistSmall *ml, int depth);
void multilistSmallSetFill(multilistSmall *ml, int fill);
void multilistSmallSetOptions(multilistSmall *ml, int fill, int depth);

/* Insert */
void multilistSmallPushByTypeHead(multilistSmall *ml, const databox *box);
void multilistSmallPushByTypeTail(multilistSmall *ml, const databox *box);

/* Bulk appending; not conforming to multilist* protocol */
void multilistSmallAppendFlex(multilistSmall *ml, const flex *fl);
void multilistSmallAppendValuesFromFlex(multilistSmall *ml, const flex *fl);
multilistSmall *multilistSmallNewFromFlex(const flex *fl);
multilistSmall *multilistSmallNewFromFlexConsume(flex *fl);

/* Insert with entry cursor */
void multilistSmallInsertByTypeBefore(multilistSmall *ml,
                                      const multilistEntry *entry,
                                      const databox *box);
void multilistSmallInsertByTypeAfter(multilistSmall *ml,
                                     const multilistEntry *entry,
                                     const databox *box);

/* Delete based on entry cursor */
void multilistSmallDelEntry(multilistIterator *iter, multilistEntry *entry);

/* Delete based on position */
bool multilistSmallDelRange(multilistSmall *ml, const mlOffsetId start,
                            const int64_t values);

/* Replace based on index */
bool multilistSmallReplaceByTypeAtIndex(multilistSmall *ml, mlOffsetId index,
                                        const databox *box);

/* Iterator generation */
void multilistSmallIteratorInit(multilistSmall *ml, multilistIterator *iter,
                                bool forward);
#define multilistSmallIteratorInitForward(ml, iter)                            \
    multilistSmallIteratorInit(ml, iter, true)
#define multilistSmallIteratorInitReverse(ml, iter)                            \
    multilistSmallIteratorInit(ml, iter, false)
bool multilistSmallIteratorInitAtIdx(multilistSmall *ml,
                                     multilistIterator *iter, mlOffsetId idx,
                                     bool forward);
#define multilistSmallIteratorInitAtIdxForward(ml, iter, idx)                  \
    multilistSmallIteratorInitAtIdx(ml, iter, idx, true)
#define multilistSmallIteratorInitAtIdxReverse(ml, iter, idx)                  \
    multilistSmallIteratorInitAtIdx(ml, iter, idx, false)

/* Iterating */
bool multilistSmallNext(multilistIterator *iter, multilistEntry *entry);

/* Reset iteration positions */
void multilistSmallRewind(multilistSmall *ml, multilistIterator *iter);
void multilistSmallRewindTail(multilistSmall *ml, multilistIterator *iter);

/* Close iterator */
void multilistSmallReleaseIterator(multilistIterator *iter);

/* Copy entire multilistSmall */
multilistSmall *multilistSmallDuplicate(multilistSmall *orig);

/* Get entry based on index */
bool multilistSmallIndex(multilistSmall *ml, mlOffsetId index,
                         multilistEntry *entry);

/* Move ends of list */
void multilistSmallRotate(multilistSmall *ml);

/* Remove and return head or tail of list */
bool multilistSmallPop(multilistSmall *ml, databox *box, bool fromTail);
#define multilistSmallPopFromTail(ml, box) multilistSmallPop(ml, box, true)
#define multilistSmallPopFromHead(ml, box) multilistSmallPop(ml, box, false)

#ifdef DATAKIT_TEST
void multilistSmallRepr(multilistSmall *ml);
int multilistSmallTest(int argc, char *argv[]);
#endif
