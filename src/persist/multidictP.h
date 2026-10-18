/* multidictP.h - Persistent multidict (hash table) wrapper
 *
 * Provides automatic WAL logging for all multidict mutations.
 * All operations mirror the standard multidict API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/mydict", NULL);
 *   multidictP *d = multidictNewP(ctx);
 *
 *   // Operations are automatically persisted
 *   databox key = DATABOX_WITH_BYTES("hello", 5);
 *   databox val = DATABOX_SIGNED(42);
 *   multidictAddP(d, &key, &val);
 *
 *   // Clean shutdown
 *   multidictCloseP(d);
 *   persistCtxFree(ctx);
 *
 * Recovery:
 *   persistCtx *ctx = persistCtxOpen("/data/mydict", NULL);
 *   multidictP *d = multidictOpenP(ctx);  // Recovers from snapshot + WAL
 */

#pragma once

#include "../multidict.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

typedef struct multidictP {
    multidict *d;    /* Underlying multidict */
    persistCtx *ctx; /* Persistence context */
} multidictP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multidict with exact keys (multidictTypeExactKey) */
multidictP *multidictNewP(persistCtx *ctx);

/* Create new persistent multidict whose keys match by 'type': one of the
 * built-in types, multidictTypeExactKey or multidictTypeCaseKey.  Returns
 * NULL for any other type, which multidictOpenP could not rebuild (see
 * persistOpsMultidict in persist.h for persisting a custom type). */
multidictP *multidictNewPWithType(persistCtx *ctx, multidictType *type);

/* Open existing persistent multidict (recover from files).  The dict
 * comes back with the key type it was created with, recorded in its
 * snapshots; files written before that record existed restore as exact
 * keys. */
multidictP *multidictOpenP(persistCtx *ctx);

/* Close persistent multidict (syncs and frees) */
void multidictCloseP(multidictP *d);

/* Get underlying multidict (for read-only operations) */
multidict *multidictGetP(multidictP *d);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

uint64_t multidictCountP(const multidictP *d);
uint64_t multidictBytesP(const multidictP *d);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Each mutation returns exactly what its multidict counterpart returns
 * for the same dict state, and its WAL record replays to the same result.
 *
 * Add or replace a key-value pair, as multidictAdd */
multidictResult multidictAddP(multidictP *d, const databox *key,
                              const databox *val);

/* Replace the value of a present key, as multidictReplace does: returns
 * MULTIDICT_OK_REPLACED, or MULTIDICT_ERR (logging nothing) when the key
 * is absent.  Use multidictAddP to insert or replace. */
multidictResult multidictReplaceP(multidictP *d, const databox *key,
                                  const databox *val);

/* Delete by key, as multidictDelete: true if found and deleted */
bool multidictDeleteP(multidictP *d, const databox *key);

/* Clear all entries */
void multidictEmptyP(multidictP *d);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Find value by key */
bool multidictFindP(multidictP *d, const databox *key, databox *val);

/* Check if key exists */
bool multidictExistsP(multidictP *d, const databox *key);

/* ============================================================================
 * Iteration (read-only)
 * ============================================================================
 */

/* Initialize iterator on underlying multidict */
bool multidictIteratorInitP(multidictP *d, multidictIterator *iter);

/* Use standard multidictIteratorNext/Release with the iterator */

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multidictSyncP(multidictP *d);

/* Force compaction now */
bool multidictCompactP(multidictP *d);

/* Get persistence statistics */
void multidictGetStatsP(const multidictP *d, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multidictPTest(int argc, char *argv[]);
#endif
