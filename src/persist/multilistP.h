/* multilistP.h - Persistent multilist wrapper
 *
 * Provides automatic WAL logging for all multilist mutations.
 * All operations mirror the standard multilist API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/mylist", NULL);
 *   multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
 *
 *   // Operations are automatically persisted
 *   databox box = DATABOX_SIGNED(42);
 *   multilistPushTailP(ml, &box);
 *
 *   // Clean shutdown
 *   multilistCloseP(ml);
 *   persistCtxFree(ctx);
 *
 * Recovery:
 *   persistCtx *ctx = persistCtxOpen("/data/mylist", NULL);
 *   multilistP *ml = multilistOpenP(ctx);  // Recovers from snapshot + WAL
 */

#pragma once

#include "../multilist.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

/* @handle: fixed: its address holds from creation to free */
typedef struct multilistP multilistP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multilist */
multilistP *multilistNewP(persistCtx *ctx, flexCapSizeLimit limit,
                          uint32_t depth);

/* Open existing persistent multilist (recover from files) */
multilistP *multilistOpenP(persistCtx *ctx);

/* Close persistent multilist (syncs and frees) */
void multilistCloseP(multilistP *ml);

/* Get underlying multilist (for read-only operations) */
multilist *multilistGetP(multilistP *ml);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t multilistCountP(const multilistP *ml);
size_t multilistBytesP(const multilistP *ml);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Every mutation logs first and applies exactly when the result is not
 * PERSIST_LOG_REFUSED (see persistLogResult).  One that would change
 * nothing (popping an empty list, a range that deletes nothing) is
 * PERSIST_LOG_UNCHANGED and logs nothing. */

/* Push to head / tail */
persistLogResult multilistPushHeadP(multilistP *ml, const databox *box);
persistLogResult multilistPushTailP(multilistP *ml, const databox *box);

/* Pop from head / tail.  When an element is popped, *got receives an
 * owned copy of it; 'got' must be zeroed or a DATABOX_BIG_INIT'd
 * databoxBig, and the result is released with databoxFreeData (see
 * multilistPop in multilist.h). */
persistLogResult multilistPopHeadP(multilistP *ml, databox *got);
persistLogResult multilistPopTailP(multilistP *ml, databox *got);

/* Delete range: values <= 0, an empty list or a start outside the list
 * deletes nothing (UNCHANGED) */
persistLogResult multilistDelRangeP(multilistP *ml, mlOffsetId start,
                                    int64_t values);

/* Replace at index: an index outside the list is REFUSED */
persistLogResult multilistReplaceAtIndexP(multilistP *ml, mlOffsetId index,
                                          const databox *box);

/* Reset (clear all entries) */
persistLogResult multilistResetP(multilistP *ml);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Index-based access */
bool multilistIndexP(const multilistP *ml, mflexState *state, mlOffsetId index,
                     multilistEntry *entry, bool openNode);
#define multilistIndexGetP(ml, s, i, e) multilistIndexP(ml, s, i, e, true)
#define multilistIndexCheckP(ml, s, i, e) multilistIndexP(ml, s, i, e, false)

/* ============================================================================
 * Iteration (read-only)
 * ============================================================================
 */

/* Initialize iterator on underlying multilist */
void multilistIteratorInitP(multilistP *ml, mflexState *state[2],
                            multilistIterator *iter, bool forward,
                            bool readOnly);
#define multilistIteratorInitForwardP(ml, s, iter)                             \
    multilistIteratorInitP(ml, s, iter, true, false)
#define multilistIteratorInitReverseP(ml, s, iter)                             \
    multilistIteratorInitP(ml, s, iter, false, false)
#define multilistIteratorInitForwardReadOnlyP(ml, s, iter)                     \
    multilistIteratorInitP(ml, s, iter, true, true)
#define multilistIteratorInitReverseReadOnlyP(ml, s, iter)                     \
    multilistIteratorInitP(ml, s, iter, false, true)

/* Use standard multilistNext/multilistIteratorRelease with the iterator */

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multilistSyncP(multilistP *ml);

/* Force compaction now */
bool multilistCompactP(multilistP *ml);

/* Get persistence statistics */
void multilistGetStatsP(const multilistP *ml, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multilistPTest(int argc, char *argv[]);
#endif
