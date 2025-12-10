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

typedef struct multilistP {
    multilist *ml;          /* Underlying multilist */
    persistCtx *ctx;        /* Persistence context */
    mflexState *state;      /* State for compressed node operations */
    flexCapSizeLimit limit; /* Cached for recreation */
    uint32_t depth;         /* Cached for recreation */
} multilistP;

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

/* Push to head - automatically persisted */
void multilistPushHeadP(multilistP *ml, const databox *box);

/* Push to tail - automatically persisted */
void multilistPushTailP(multilistP *ml, const databox *box);

/* Pop from head - automatically persisted */
bool multilistPopHeadP(multilistP *ml, databox *got);

/* Pop from tail - automatically persisted */
bool multilistPopTailP(multilistP *ml, databox *got);

/* Delete range - automatically persisted */
bool multilistDelRangeP(multilistP *ml, mlOffsetId start, int64_t values);

/* Replace at index - automatically persisted */
bool multilistReplaceAtIndexP(multilistP *ml, mlNodeId index,
                              const databox *box);

/* Reset (clear all entries) - automatically persisted */
void multilistResetP(multilistP *ml);

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
