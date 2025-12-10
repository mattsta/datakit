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

/* Create new persistent multidict */
multidictP *multidictNewP(persistCtx *ctx);

/* Open existing persistent multidict (recover from files) */
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

/* Add key-value pair - returns result code */
multidictResult multidictAddP(multidictP *d, const databox *key,
                              const databox *val);

/* Replace value for key (insert if not exists) */
multidictResult multidictReplaceP(multidictP *d, const databox *key,
                                  const databox *val);

/* Delete by key - returns true if found and deleted */
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
