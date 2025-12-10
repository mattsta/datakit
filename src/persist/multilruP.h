/* multilruP.h - Persistent multilru wrapper
 *
 * Provides automatic WAL logging for all multilru mutations.
 * All operations mirror the standard multilru API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/mycache", NULL);
 *   multilruP *cache = multilruNewP(ctx, 7, 10000);  // 7 levels, 10K entries
 * max
 *
 *   // Operations are automatically persisted
 *   multilruPtr handle = multilruInsertP(cache);
 *   multilruIncreaseP(cache, handle);  // Promote on cache hit
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

typedef struct multilruP {
    multilru *mlru;     /* Underlying multilru */
    persistCtx *ctx;    /* Persistence context */
    bool enableWeights; /* Track if weights are enabled (for snapshot) */
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

/* Insert new entry - returns handle */
multilruPtr multilruInsertP(multilruP *mlru);

/* Insert with weight */
multilruPtr multilruInsertWeightedP(multilruP *mlru, uint64_t weight);

/* Promote entry to next level (on cache hit) */
void multilruIncreaseP(multilruP *mlru, multilruPtr ptr);

/* Update weight of existing entry */
void multilruUpdateWeightP(multilruP *mlru, multilruPtr ptr,
                           uint64_t newWeight);

/* Remove LRU entry with S4LRU demotion */
bool multilruRemoveMinimumP(multilruP *mlru, multilruPtr *out);

/* Delete specific entry immediately */
void multilruDeleteP(multilruP *mlru, multilruPtr ptr);

/* ============================================================================
 * Configuration (changes are persisted via snapshot on next compact)
 * ============================================================================
 */

void multilruSetMaxCountP(multilruP *mlru, uint64_t maxCount);
uint64_t multilruGetMaxCountP(const multilruP *mlru);

void multilruSetMaxWeightP(multilruP *mlru, uint64_t maxWeight);
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
