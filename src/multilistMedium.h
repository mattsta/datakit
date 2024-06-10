/* multilistMedium.h - medium growable list interface
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

typedef struct multilistMedium multilistMedium;

/* Create */
multilistMedium *multilistMediumCreate(void);
multilistMedium *multilistMediumNew(int fill, int compress);

/* Free */
void multilistMediumFree(multilistMedium *ml);

/* Metadata */
size_t multilistMediumCount(const multilistMedium *ml);
size_t multilistMediumBytes(const multilistMedium *ml);

/* Settings */
void multilistMediumSetCompressDepth(multilistMedium *ml, int depth);
void multilistMediumSetFill(multilistMedium *ml, int fill);
void multilistMediumSetOptions(multilistMedium *ml, int fill, int depth);

/* Insert */
void multilistMediumPushByTypeHead(multilistMedium *ml, const databox *box);
void multilistMediumPushByTypeTail(multilistMedium *ml, const databox *box);

/* Bulk appending; not conforming to multilist* protocol */
void multilistMediumAppendFlex(multilistMedium *ml, const flex *fl);
void multilistMediumAppendValuesFromFlex(multilistMedium *ml, const flex *fl);
multilistMedium *multilistMediumNewFromFlex(const flex *fl);
multilistMedium *multilistMediumNewFromFlexConsume(flex *fl);
multilistMedium *multilistMediumNewFromFlexConsumeGrow(void *_ml, flex *fl);

/* Insert with entry cursor */
void multilistMediumInsertByTypeBefore(multilistMedium *ml,
                                       const multilistEntry *entry,
                                       const databox *box);
void multilistMediumInsertByTypeAfter(multilistMedium *ml,
                                      const multilistEntry *entry,
                                      const databox *box);

/* Delete based on entry cursor */
void multilistMediumDelEntry(multilistIterator *iter, multilistEntry *entry);

/* Delete based on position */
bool multilistMediumDelRange(multilistMedium *ml, const mlOffsetId start,
                             const int64_t values);

/* Replace based on index */
bool multilistMediumReplaceByTypeAtIndex(multilistMedium *ml, mlOffsetId index,
                                         const databox *box);

/* Iterator generation */
void multilistMediumIteratorInit(multilistMedium *ml, multilistIterator *iter,
                                 bool forward);
#define multilistMediumIteratorInitForward(ml, iter)                           \
    multilistMediumIteratorInit(ml, iter, true)
#define multilistMediumIteratorInitReverse(ml, iter)                           \
    multilistMediumIteratorInit(ml, iter, false)
bool multilistMediumIteratorInitAtIdx(multilistMedium *ml,
                                      multilistIterator *iter, mlOffsetId idx,
                                      bool forward);
#define multilistMediumIteratorInitAtIdxForward(ml, iter, idx)                 \
    multilistMediumIteratorInitAtIdx(ml, iter, idx, true)
#define multilistMediumIteratorInitAtIdxReverse(ml, iter, idx)                 \
    multilistMediumIteratorInitAtIdx(ml, iter, idx, false)

/* Iterating */
bool multilistMediumNext(multilistIterator *iter, multilistEntry *entry);

/* Reset iteration positions */
void multilistMediumRewind(multilistMedium *ml, multilistIterator *iter);
void multilistMediumRewindTail(multilistMedium *ml, multilistIterator *iter);

/* Close iterator */
void multilistMediumReleaseIterator(multilistIterator *iter);

/* Copy entire multilistMedium */
multilistMedium *multilistMediumDuplicate(multilistMedium *orig);

/* Get entry based on index */
bool multilistMediumIndex(multilistMedium *ml, mlOffsetId index,
                          multilistEntry *entry);

/* Move ends of list */
void multilistMediumRotate(multilistMedium *ml);

/* Remove and return head or tail of list */
bool multilistMediumPop(multilistMedium *ml, databox *box, bool fromTail);
#define multilistMediumPopFromTail(ml, box) multilistMediumPop(ml, box, true)
#define multilistMediumPopFromHead(ml, box) multilistMediumPop(ml, box, false)

#ifdef DATAKIT_TEST
void multilistMediumRepr(multilistMedium *ml);
int multilistMediumTest(int argc, char *argv[]);
#endif
