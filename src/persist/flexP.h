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

typedef struct flexP {
    flex *f;         /* Underlying flex */
    persistCtx *ctx; /* Persistence context */
} flexP;

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

/* Push to head - automatically persisted */
void flexPushHeadP(flexP *f, const databox *box);

/* Push to tail - automatically persisted */
void flexPushTailP(flexP *f, const databox *box);

/* Reset (clear all entries) - automatically persisted */
void flexResetP(flexP *f);

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
