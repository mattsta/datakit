/* flexP.h - Persistent flex wrapper
 *
 * Provides automatic WAL logging for all flex mutations.
 * All operations mirror the standard flex API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/myflex", NULL);
 *   flexP *f = flexNewP(ctx);
 *
 *   // Operations are automatically persisted
 *   databox box = DATABOX_SIGNED(42);
 *   flexPushTailP(f, &box);
 *
 *   // Clean shutdown
 *   flexCloseP(f);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../flex.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

/* @handle: fixed: its address holds from creation to free */
typedef struct flexP flexP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent flex */
flexP *flexNewP(persistCtx *ctx);

/* Open existing persistent flex (recover from files) */
flexP *flexOpenP(persistCtx *ctx);

/* Close persistent flex (syncs and frees) */
void flexCloseP(flexP *f);

/* Get underlying flex (for read-only operations) */
flex *flexGetP(flexP *f);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t flexCountP(const flexP *f);
size_t flexBytesP(const flexP *f);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Every mutation logs first and applies exactly when the result is not
 * PERSIST_LOG_REFUSED (see persistLogResult: NOT_DURABLE means applied and
 * logged, the sync failed).  Values are stored exactly as given, as
 * recovery replays them. */

/* Push to head */
persistLogResult flexPushHeadP(flexP *f, const databox *box);

/* Push to tail */
persistLogResult flexPushTailP(flexP *f, const databox *box);

/* Reset (clear all entries) */
persistLogResult flexResetP(flexP *f);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Get entry at index */
flexEntry *flexIndexP(const flexP *f, int32_t index);

/* Get entry value */
void flexGetByTypeP(const flexP *f, flexEntry *fe, databox *box);

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool flexSyncP(flexP *f);

/* Force compaction now */
bool flexCompactP(flexP *f);

/* Get persistence statistics */
void flexGetStatsP(const flexP *f, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int flexPTest(int argc, char *argv[]);
#endif
