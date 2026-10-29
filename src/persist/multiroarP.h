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
 *   multiroarBitSetP(r, 42, NULL);  // optional bool *added
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

/* @handle: fixed: its address holds from creation to free */
typedef struct multiroarP multiroarP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multiroar */
multiroarP *multiroarNewP(persistCtx *ctx);

/* Create a new persistent value-mode multiroar (see multiroarValueNew);
 * NULL for a width outside 1..64 */
multiroarP *multiroarValueNewP(persistCtx *ctx, uint8_t bitWidth, uint64_t rows,
                               uint64_t cols);

/* Open existing persistent multiroar (recover from files; either mode) */
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

/* Every mutation logs first and applies exactly when the result is not
 * PERSIST_LOG_REFUSED (see persistLogResult).  One that would change
 * nothing (setting a set bit, removing an absent one, an empty range,
 * removing an absent value) is PERSIST_LOG_UNCHANGED and logs nothing.
 * The optional 'changed' out-parameters report whether the roar changed. */

/* Set the bit at position */
persistLogResult multiroarBitSetP(multiroarP *r, uint64_t position,
                                  bool *added);

/* Clear the bit at position */
persistLogResult multiroarBitClearP(multiroarP *r, uint64_t position,
                                    bool *cleared);

/* Set / clear the bits in [start, start+extent): one log row each */
persistLogResult multiroarBitSetRangeP(multiroarP *r, uint64_t start,
                                       uint64_t extent);
persistLogResult multiroarBitClearRangeP(multiroarP *r, uint64_t start,
                                         uint64_t extent);

/* Value mode: store `value` at `position` / remove the value there.
 * REFUSED for a bit-mode roar, a value that is not an integer of the
 * roar's width or a position outside its dimensions. */
persistLogResult multiroarValueSetP(multiroarP *r, uint64_t position,
                                    const databox *value);
persistLogResult multiroarValueRemoveP(multiroarP *r, uint64_t position,
                                       bool *removed);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Check if bit is set */
bool multiroarBitGetP(const multiroarP *r, uint64_t position);

/* Value mode: the value at `position` (false when there is none) and the
 * count of stored values */
bool multiroarValueGetP(const multiroarP *r, uint64_t position, databox *value);
uint64_t multiroarValueCountP(const multiroarP *r);

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
