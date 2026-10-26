#pragma once

#include "dkTier.h"
#include "dkTierLimit.h"
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

/* multilistNew: the process default limit for multilists (dkTierLimit.h),
 * no compression.  multilistNewLimit: an explicit limit, and 'depth' nodes
 * kept uncompressed at each end (0: no compression). */
[[nodiscard]] multilist *multilistNew(void);
[[nodiscard]] multilist *multilistNewLimit(flexCapSizeLimit limit,
                                           uint32_t depth);
[[nodiscard]] multilist *multilistNewFromFlex(flexCapSizeLimit limit,
                                              uint32_t depth, flex *fl);
[[nodiscard]] multilist *multilistCopy(const multilist *ml);
/* A multilist of every element of 'elements' in order (as multilistDump
 * writes them), built straight into its tier: a large dump is cut into
 * node-sized pieces in O(n), with no per-element push.  Takes 'elements';
 * 'state' is scratch for compressing interior nodes. */
multilist *multilistNewFromDump(flexCapSizeLimit limit, uint32_t depth,
                                mflexState *state, flex *elements);

/* optionals
void multilistSetCompressDepth(multilist *ml, int depth);
void multilistSetFill(multilist *ml, int fill);
void multilistSetOptions(multilist *ml, int fill, int depth);
*/

size_t multilistCount(const multilist *m);
size_t multilistBytes(const multilist *m);
/* The tier, nodes (1, 2, or Full's count), elements, and bytes: the
 * nodes as stored (Full's compressed nodes at their compressed size), and
 * those plus each tier's struct and Full's node array.  'unitLimit' is
 * where Small grows (the size limit's cutoff) or Full's nodes split (0
 * when Full has no size fill). */
void multilistStats(const multilist *m, dkTierStats *out);

/* Every element, in list order, as one new flex; release with flexFree.
 * The element encodings are copied byte for byte and never decoded, so
 * the result is identical to pushing each element onto an empty flex in
 * order, 96/128-bit values included.  'state' is the scratch space for
 * opening compressed Full-tier nodes; Small and Medium ignore it. */
flex *multilistDump(const multilist *m, mflexState *state);

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

/* Delete the element 'entry' names, which multilistNext just returned
 * from the writable iterator 'iter'.  The walk then goes on with the
 * element after it (forward) or before it (reverse), and the iterator's
 * offset keeps naming that element, so later entries carry right
 * offsets. */
void multilistDelEntry(multilistIterator *iter, multilistEntry *entry);

/* Delete up to 'values' elements starting at index 'start' (negative
 * counts from the tail), clamped to the end of the list.  Returns true
 * if anything was deleted.  A count of zero or less, an empty list, or
 * a start outside the list deletes nothing and returns false, on every
 * tier. */
bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values);

/* Replace the element at 'index' (negative counts from the tail) with
 * 'box'.  Returns false, changing nothing, when 'index' is outside the
 * list, an empty list included.  'index' is a full-width list index, so
 * no index is ever truncated onto another element. */
bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlOffsetId index, const databox *box);

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
    multilistIteratorInitAtIdx(ml, s, iter, idx, true, false)
#define multilistIteratorInitAtIdxForwardReadOnly(ml, s, iter, idx)            \
    multilistIteratorInitAtIdx(ml, s, iter, idx, true, true)
#define multilistIteratorInitAtIdxReverse(ml, s, iter, idx)                    \
    multilistIteratorInitAtIdx(ml, s, iter, idx, false, false)
#define multilistIteratorInitAtIdxReverseReadOnly(ml, s, iter, idx)            \
    multilistIteratorInitAtIdx(ml, s, iter, idx, false, true)

/* multilistNext fills 'entry' (it needs no initialization) and decodes the
 * element into entry->box.  A 96/128-bit element reads back whole, with
 * its payload in iter->wide, valid until the next multilistNext on 'iter'.
 * entry->box borrows from the list and from the iterator; see
 * multilistEntry in multilistCommon.h for how long it stays valid. */
bool multilistNext(multilistIterator *iter, multilistEntry *entry);

void multilistRewind(multilist *ml, multilistIterator *iter);
void multilistRewindTail(multilist *ml, multilistIterator *iter);

void multilistIteratorRelease(multilistIterator *iter);

/* multilistIndex fills 'entry' (it needs no initialization) with the
 * element at 'index'; negative indexes count from the tail.  A 96/128-bit
 * element is reported deferred: type only, data.ptr = NULL
 * (databoxIsWideDeferred), because a single read has no storage of its
 * own for the payload.  Use multilistIndexWide when the value is needed.
 * With openNode false the Full tier only checks that 'index' exists and
 * leaves entry->fe and entry->box unset; Small and Medium always decode.
 * An index outside the list (index >= count or index < -count, any index
 * of an empty list) returns false and leaves 'entry' unusable, with one
 * exception: on an empty list multilistIndexGet aims 'entry' at the empty
 * head, so InsertByTypeBefore/After can add the first element with it. */
bool multilistIndex(const multilist *ml, mflexState *state, mlOffsetId index,
                    multilistEntry *entry, bool openNode);
#define multilistIndexGet(ml, s, i, e) multilistIndex(ml, s, i, e, true)
#define multilistIndexCheck(ml, s, i, e) multilistIndex(ml, s, i, e, false)

/* multilistIndexWide is multilistIndexGet that reads a 96/128-bit element
 * whole: its payload goes to 'slot' (big = 1, allocated = 0), so
 * entry->box is valid for as long as 'slot' is and the list is unchanged.
 * Narrow elements decode exactly as with multilistIndexGet and never
 * touch 'slot'.  The entry is a full result, usable as an IndexNear
 * anchor or with DelEntry-style cursor calls. */
bool multilistIndexWide(const multilist *ml, mflexState *state,
                        mlOffsetId index, multilistEntry *entry,
                        databoxWide *slot);

/* Index starting from a caller-held anchor entry (with its known global
 * index) when the anchor is closer than either endpoint; on the Full
 * tier whole nodes are skipped by their counts from the anchor's node
 * in either direction.  The anchor must have been obtained after the
 * most recent mutation.  Never slower than multilistIndex.  A 96/128-bit
 * element is reported deferred, as with multilistIndex;
 * multilistIndexNearWide reads it whole into 'slot', as
 * multilistIndexWide does. */
bool multilistIndexNear(const multilist *ml, mflexState *state,
                        mlOffsetId index, const multilistEntry *anchor,
                        mlOffsetId anchorIdx, multilistEntry *entry,
                        bool openNode);
#define multilistIndexNearGet(ml, s, i, a, ai, e)                              \
    multilistIndexNear(ml, s, i, a, ai, e, true)
#define multilistIndexNearCheck(ml, s, i, a, ai, e)                            \
    multilistIndexNear(ml, s, i, a, ai, e, false)
bool multilistIndexNearWide(const multilist *ml, mflexState *state,
                            mlOffsetId index, const multilistEntry *anchor,
                            mlOffsetId anchorIdx, multilistEntry *entry,
                            databoxWide *slot);

void multilistRotate(multilist *m, mflexState *state[2]);

/* Remove the head or tail element and return it in *got as a
 * self-contained value, on every tier:
 *   - BYTES of 8 bytes or less are embedded (DATABOX_BYTES_EMBED,
 *     allocated = 0), so read them with databoxBytes / databoxCBytes;
 *   - longer BYTES and containers are heap copies (allocated = 1);
 *   - a 96/128-bit value lands in the storage got->data points at when
 *     got->big is set (a DATABOX_BIG_INIT'd databoxBig), and otherwise in
 *     a 16-byte heap block (allocated = 1, big = 1);
 *   - every other value has allocated = 0 and big = 0.
 * Popping a narrow value (an integer, a float or a short string) never
 * allocates.
 * 'got' must be zeroed or a DATABOX_BIG_INIT'd databoxBig, because its
 * 'big' flag is read.  Release the result with databoxFreeData.  Returns
 * false, leaving *got untouched, when the list is empty. */
bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail);
#define multilistPopTail(ml, s, box) multilistPop(ml, s, box, true)
#define multilistPopHead(ml, s, box) multilistPop(ml, s, box, false)

void multilistFree(multilist *m);
/* Remove every element, keeping the node size limit and compression
 * depth the list was created with */
void multilistReset(multilist **ml);
/* Pack the values into the densest layout they allow: Full's values are
 * repacked, in order, into full nodes (every node but the last within one
 * value of the limit; interior nodes compressed per the depth), then the
 * list moves to the smallest tier that holds its bytes without growing
 * (Small at L or less, Medium at 3L or less).  O(values); beyond the old
 * and new nodes it needs about one node of scratch.  'state' opens and
 * compresses nodes.  Rewrites '*ml'. */
void multilistCompact(multilist **ml, mflexState *state);
/* The node size limit and compression depth the list was created with */
flexCapSizeLimit multilistLimit(const multilist *ml);
uint32_t multilistDepth(const multilist *ml);

/* Bulk appending — use for growing
multilist *multilistlNewFromFlex(int fill, int compress, flex *fl);
*/

#ifdef DATAKIT_TEST
void multilistRepr(const multilist *m);
int multilistTest(int argc, char *argv[]);
#endif
