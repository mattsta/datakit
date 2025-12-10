/* intsetP.h - Persistent intset wrapper
 *
 * Provides automatic WAL logging for all intset mutations.
 * All operations mirror the standard intset API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/myintset", NULL);
 *   intsetP *is = intsetNewP(ctx);
 *
 *   // Operations are automatically persisted
 *   intsetAddP(is, 42);
 *
 *   // Clean shutdown
 *   intsetCloseP(is);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../intset.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

typedef struct intsetP {
    intset *is;      /* Underlying intset */
    persistCtx *ctx; /* Persistence context */
} intsetP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent intset */
intsetP *intsetNewP(persistCtx *ctx);

/* Open existing persistent intset (recover from files) */
intsetP *intsetOpenP(persistCtx *ctx);

/* Close persistent intset (syncs and frees) */
void intsetCloseP(intsetP *is);

/* Get underlying intset (for read-only operations) */
intset *intsetGetRawP(intsetP *is);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

uint64_t intsetCountP(const intsetP *is);
size_t intsetBytesP(const intsetP *is);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Add value - returns true if added, false if already exists */
bool intsetAddP(intsetP *is, int64_t value);

/* Remove value - returns true if removed, false if not found */
bool intsetRemoveP(intsetP *is, int64_t value);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Check if value exists */
bool intsetFindP(const intsetP *is, int64_t value);

/* Get value at index */
bool intsetGetAtP(const intsetP *is, uint32_t pos, int64_t *value);

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool intsetSyncP(intsetP *is);

/* Force compaction now */
bool intsetCompactP(intsetP *is);

/* Get persistence statistics */
void intsetGetStatsP(const intsetP *is, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int intsetPTest(int argc, char *argv[]);
#endif
