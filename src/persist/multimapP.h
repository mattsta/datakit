/* multimapP.h - Persistent multimap wrapper
 *
 * Provides automatic WAL logging for all multimap mutations.
 * All operations mirror the standard multimap API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/mymap", NULL);
 *   multimapP *m = multimapNewP(ctx, 2);  // key + value
 *
 *   // Operations are automatically persisted
 *   multimapInsertP(m, entries, NULL);  // optional bool *replaced
 *   multimapDeleteP(m, &key);
 *
 *   // Clean shutdown
 *   multimapCloseP(m);
 *   persistCtxFree(ctx);
 *
 * Recovery:
 *   persistCtx *ctx = persistCtxOpen("/data/mymap", NULL);
 *   multimapP *m = multimapOpenP(ctx);  // Recovers from snapshot + WAL
 */

#pragma once

#include "../multimap.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

/* @handle: fixed: its address holds from creation to free */
typedef struct multimapP multimapP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multimap */
multimapP *multimapNewP(persistCtx *ctx, uint32_t elementsPerEntry);

/* Create new persistent multimap with custom configuration */
multimapP *multimapNewConfigureP(persistCtx *ctx, uint32_t elementsPerEntry,
                                 bool isSet, bool compress,
                                 flexCapSizeLimit limit);

/* Open existing persistent multimap (recover from files) */
multimapP *multimapOpenP(persistCtx *ctx);

/* Close persistent multimap (syncs and frees) */
void multimapCloseP(multimapP *m);

/* Get underlying multimap (for read-only operations) */
multimap *multimapGetP(multimapP *m);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t multimapCountP(const multimapP *m);
size_t multimapBytesP(const multimapP *m);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Every mutation logs first and applies exactly when the result is not
 * PERSIST_LOG_REFUSED (see persistLogResult).  One that would change
 * nothing (an existing key for IfNotExists, deleting an absent key or row,
 * an increment by 0) is PERSIST_LOG_UNCHANGED and logs nothing.  The
 * optional out-parameters report what changed. */

/* Insert or replace an entry ('replaced': the key existed) */
persistLogResult multimapInsertP(multimapP *m, const databox *elements[],
                                 bool *replaced);

/* Insert only if the key is absent (UNCHANGED when it exists) */
persistLogResult multimapInsertIfNotExistsP(multimapP *m,
                                            const databox *elements[]);

/* Delete by key / the full-width row (for sets) */
persistLogResult multimapDeleteP(multimapP *m, const databox *key);
persistLogResult multimapDeleteFullWidthP(multimapP *m,
                                          const databox *elements[]);

/* Reset (clear all entries), then compact */
persistLogResult multimapResetP(multimapP *m);

/* multimapFieldIncrBy, logged as the increment (not the result) so replay
 * folds it again.  An increment the map refuses (a missing key, a field
 * that is not a counter, a result outside int64) is REFUSED before
 * anything is logged.  '*newValue' (may be NULL) gets the new value, or
 * the current one (0 for a missing key or a non-counter) when nothing
 * changed. */
persistLogResult multimapFieldIncrP(multimapP *m, const databox *key,
                                    uint32_t fieldOffset, int64_t incrBy,
                                    int64_t *newValue);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* These just delegate to the underlying multimap */
bool multimapLookupP(multimapP *m, const databox *key, databox *elements[]);
bool multimapExistsP(multimapP *m, const databox *key);
bool multimapFirstP(multimapP *m, databox *elements[]);
bool multimapLastP(multimapP *m, databox *elements[]);
bool multimapRandomValueP(multimapP *m, bool fromTail, databox **foundBox,
                          multimapEntry *me);

/* ============================================================================
 * Iteration (read-only)
 * ============================================================================
 */

/* Iterator operates on underlying multimap */
void multimapIteratorInitP(multimapP *m, multimapIterator *iter, bool forward);
bool multimapIteratorInitAtP(multimapP *m, multimapIterator *iter, bool forward,
                             const databox *startAt);
/* Use standard multimapIteratorNext with the iterator */

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multimapSyncP(multimapP *m);

/* Force compaction now */
bool multimapCompactP(multimapP *m);

/* Get persistence statistics */
void multimapGetStatsP(const multimapP *m, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multimapPTest(int argc, char *argv[]);
#endif
