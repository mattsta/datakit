/* multilruP.h - Persistent multilru wrapper
 *
 * Provides automatic WAL logging for all multilru mutations.
 * All operations mirror the standard multilru API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/mycache", NULL);
 *   // 7 levels, at most 10K entries
 *   multilruP *cache = multilruNewWithLevelsP(ctx, 7, 10000);
 *
 *   // Operations are automatically persisted
 *   multilruPtr handle = 0;
 *   if (multilruInsertP(cache, &handle) != PERSIST_LOG_REFUSED) {
 *       multilruIncreaseP(cache, handle);  // Promote on cache hit
 *   }
 *
 *   // Clean shutdown
 *   multilruCloseP(cache);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../multilru.h"
#include "persistCtx.h"
#include <sys/types.h> /* For ssize_t used in multilru.h */

/* ============================================================================
 * Types
 * ============================================================================
 */

/* The wrapped multilru's auto-eviction is off: each insert logs itself,
 * then enforces the limits one logged step at a time (a demotion or an
 * eviction), so replay never runs the eviction policy. */
typedef struct multilruP {
    multilru *mlru;  /* Underlying multilru */
    persistCtx *ctx; /* Persistence context */
} multilruP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multilru with default settings (7 levels, no limits) */
multilruP *multilruNewP(persistCtx *ctx);

/* Create with specified levels and max count */
multilruP *multilruNewWithLevelsP(persistCtx *ctx, size_t maxLevels,
                                  uint64_t maxCount);

/* Create with full configuration control */
multilruP *multilruNewWithConfigP(persistCtx *ctx,
                                  const multilruConfig *config);

/* Open existing persistent multilru (recover from files) */
multilruP *multilruOpenP(persistCtx *ctx);

/* Close persistent multilru (syncs and frees) */
void multilruCloseP(multilruP *mlru);

/* Get underlying multilru (for read-only operations) */
multilru *multilruGetRawP(multilruP *mlru);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t multilruCountP(const multilruP *mlru);
size_t multilruBytesP(const multilruP *mlru);
uint64_t multilruTotalWeightP(const multilruP *mlru);
size_t multilruLevelCountP(const multilruP *mlru, size_t level);
uint64_t multilruLevelWeightP(const multilruP *mlru, size_t level);
uint64_t multilruGetWeightP(const multilruP *mlru, multilruPtr ptr);
size_t multilruGetLevelP(const multilruP *mlru, multilruPtr ptr);
bool multilruIsPopulatedP(const multilruP *mlru, multilruPtr ptr);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Every mutation logs each step before taking it and applies exactly when
 * the result is not PERSIST_LOG_REFUSED (see persistLogResult); a step
 * the limits force (eviction, demotion) is logged too, and the result is
 * the least durable of the steps.  A change to an entry that is not live,
 * a weight that is already set, or removing from an empty LRU is
 * PERSIST_LOG_UNCHANGED and logs nothing. */

/* Insert a new entry; '*handle' (may be NULL) gets it, or 0 when the
 * limits evicted it at once */
persistLogResult multilruInsertP(multilruP *mlru, multilruPtr *handle);
persistLogResult multilruInsertWeightedP(multilruP *mlru, uint64_t weight,
                                         multilruPtr *handle);

/* Promote entry to next level (on cache hit) */
persistLogResult multilruIncreaseP(multilruP *mlru, multilruPtr ptr);

/* Update the weight of a live entry, then evict to the limits */
persistLogResult multilruUpdateWeightP(multilruP *mlru, multilruPtr ptr,
                                       uint64_t newWeight);

/* Take one S4LRU step ('*out': the entry; multilruIsPopulated afterwards
 * tells a demotion from an eviction) */
persistLogResult multilruRemoveMinimumP(multilruP *mlru, multilruPtr *out);

/* Delete specific entry immediately */
persistLogResult multilruDeleteP(multilruP *mlru, multilruPtr ptr);

/* ============================================================================
 * Configuration: a new limit is applied, evicted to at once (each step
 * logged) and made durable by a snapshot (NOT_DURABLE when that fails);
 * REFUSED, with nothing changed, whenever a logged change would be (a WAL
 * past a bad entry, until an explicit compaction)
 * ============================================================================
 */

persistLogResult multilruSetMaxCountP(multilruP *mlru, uint64_t maxCount);
uint64_t multilruGetMaxCountP(const multilruP *mlru);

persistLogResult multilruSetMaxWeightP(multilruP *mlru, uint64_t maxWeight);
uint64_t multilruGetMaxWeightP(const multilruP *mlru);

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multilruSyncP(multilruP *mlru);

/* Force compaction now */
bool multilruCompactP(multilruP *mlru);

/* Get persistence statistics */
void multilruGetStatsP(const multilruP *mlru, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multilruPTest(int argc, char *argv[]);
#endif
