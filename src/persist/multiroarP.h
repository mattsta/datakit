/* multiroarP.h - Persistent multiroar wrapper
 *
 * Provides automatic WAL logging for all multiroar mutations.
 * All operations mirror the standard multiroar API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/myroar", NULL);
 *   multiroarP *r = multiroarNewP(ctx);
 *
 *   // Operations are automatically persisted
 *   multiroarBitSetP(r, 42);
 *
 *   // Clean shutdown
 *   multiroarCloseP(r);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../multiroar.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

typedef struct multiroarP {
    multiroar *r;    /* Underlying multiroar */
    persistCtx *ctx; /* Persistence context */
} multiroarP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multiroar */
multiroarP *multiroarNewP(persistCtx *ctx);

/* Open existing persistent multiroar (recover from files) */
multiroarP *multiroarOpenP(persistCtx *ctx);

/* Close persistent multiroar (syncs and frees) */
void multiroarCloseP(multiroarP *r);

/* Get underlying multiroar (for read-only operations) */
multiroar *multiroarGetRawP(multiroarP *r);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t multiroarBitCountP(const multiroarP *r);
size_t multiroarMemoryUsageP(const multiroarP *r);
bool multiroarIsEmptyP(const multiroarP *r);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Set bit at position - returns true if bit was not previously set */
bool multiroarBitSetP(multiroarP *r, size_t position);

/* Remove bit at position - returns true if bit was previously set */
bool multiroarRemoveP(multiroarP *r, size_t position);

/* Set range of bits [start, start+extent) */
void multiroarBitSetRangeP(multiroarP *r, size_t start, size_t extent);

/* Clear range of bits [start, start+extent) */
void multiroarBitClearRangeP(multiroarP *r, uint64_t start, uint64_t extent);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Check if bit is set */
bool multiroarBitGetP(const multiroarP *r, size_t position);

/* Get min/max set bits */
bool multiroarMinP(const multiroarP *r, uint64_t *position);
bool multiroarMaxP(const multiroarP *r, uint64_t *position);

/* Rank/Select operations */
size_t multiroarRankP(const multiroarP *r, uint64_t position);
bool multiroarSelectP(const multiroarP *r, size_t k, uint64_t *position);

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multiroarSyncP(multiroarP *r);

/* Force compaction now */
bool multiroarCompactP(multiroarP *r);

/* Get persistence statistics */
void multiroarGetStatsP(const multiroarP *r, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multiroarPTest(int argc, char *argv[]);
#endif
