#pragma once

#include "flex.h"
#include "flexCapacityManagement.h"
#include "multilistCommon.h"
#include <stdbool.h>
#include <stddef.h>

/* TODO:
 *   - define common multilist iterator / entry struct.
 *      - use with small/full too.
 *   - flesh out functions
 *   - flesh out tests
 *   - how to induce compression on promotion?  explicit extra config param on
 * insert? */

/* opaque multilist type; there's no user accessible data here. */
typedef struct multilist multilist;

multilist *multilistNew(flexCapSizeLimit limit, uint32_t depth);
multilist *multilistNewFromFlex(flexCapSizeLimit limit, uint32_t depth,
                                flex *fl);
multilist *multilistDuplicate(const multilist *ml);

/* optionals
void multilistSetCompressDepth(multilist *ml, int depth);
void multilistSetFill(multilist *ml, int fill);
void multilistSetOptions(multilist *ml, int fill, int depth);
*/

size_t multilistCount(const multilist *m);
size_t multilistBytes(const multilist *m);

/* TODO: add multi-argument versions so we can, e.g. append
 *       40 databoxes at once as efficiently as possible. */
void multilistPushByTypeHead(multilist **m, mflexState *state,
                             const databox *box);
void multilistPushByTypeTail(multilist **m, mflexState *state,
                             const databox *box);

#if 1
void multilistInsertByTypeAfter(multilist **m, mflexState *state[2],
                                multilistEntry *node, const databox *box);
void multilistInsertByTypeBefore(multilist **m, mflexState *state[2],
                                 multilistEntry *node, const databox *box);
#endif

void multilistDelEntry(multilistIterator *iter, multilistEntry *entry);
bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values);
bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlNodeId index, const databox *box);

void multilistIteratorInit(multilist *ml, mflexState *state[2],
                           multilistIterator *iter, bool forward,
                           bool readOnly);
#define multilistIteratorInitReadOnly(ml, s, iter, forward)                    \
    multilistIteratorInit(ml, s, iter, forward, true)
#define multilistIteratorInitForwardReadOnly(ml, s, iter)                      \
    multilistIteratorInit(ml, s, iter, true, true)
#define multilistIteratorInitForward(ml, s, iter)                              \
    multilistIteratorInit(ml, s, iter, true, false)
#define multilistIteratorInitReverse(ml, s, iter)                              \
    multilistIteratorInit(ml, s, iter, false, false)
#define multilistIteratorInitReverseReadOnly(ml, s, iter)                      \
    multilistIteratorInit(ml, s, iter, false, true)

bool multilistIteratorInitAtIdx(const multilist *ml, mflexState *state[2],
                                multilistIterator *iter, mlOffsetId idx,
                                bool forward, bool readOnly);
#define multilistIteratorInitAtIdxForward(ml, s, iter, idx)                    \
    multilistIteratorInitAtIdx(ml, s, iter, idx, true)
#define multilistIteratorInitAtIdxForwardReadOnly(ml, s, iter, idx)            \
    multilistIteratorInitAtIdx(ml, s, iter, idx, true, true)
#define multilistIteratorInitAtIdxReverse(ml, s, iter, idx)                    \
    multilistIteratorInitAtIdx(ml, s, iter, idx, false)
#define multilistIteratorInitAtIdxReverseReadOnly(ml, s, iter, idx)            \
    multilistIteratorInitAtIdx(ml, s, iter, idx, false, true)

bool multilistNext(multilistIterator *iter, multilistEntry *entry);

void multilistRewind(multilist *ml, multilistIterator *iter);
void multilistRewindTail(multilist *ml, multilistIterator *iter);

void multilistIteratorRelease(multilistIterator *iter);

bool multilistIndex(const multilist *ml, mflexState *state, mlOffsetId index,
                    multilistEntry *entry, bool openNode);
#define multilistIndexGet(ml, s, i, e) multilistIndex(ml, s, i, e, true)
#define multilistIndexCheck(ml, s, i, e) multilistIndex(ml, s, i, e, false)

void multilistRotate(multilist *m, mflexState *state[2]);
bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail);
#define multilistPopTail(ml, s, box) multilistPop(ml, s, box, true)
#define multilistPopHead(ml, s, box) multilistPop(ml, s, box, false)

void multilistFree(multilist *m);

/* Bulk appending — use for growing
void multilistAppendFlex(multilist *ml, flex *fl);
multilist *multilistAppendValuesFromFlex(multilist *ml, flex *fl);
multilist *multilistlNewFromFlex(int fill, int compress, flex *fl);
*/

#ifdef DATAKIT_TEST
void multilistRepr(const multilist *m);
int multilistTest(int argc, char *argv[]);
#endif
