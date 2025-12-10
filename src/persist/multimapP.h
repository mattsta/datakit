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
 *   multimapInsertP(m, entries);
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

typedef struct multimapP {
    multimap *m;               /* Underlying multimap */
    persistCtx *ctx;           /* Persistence context */
    uint32_t elementsPerEntry; /* Cached for encoding */
} multimapP;

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

/* Insert entry - returns true if replaced existing */
bool multimapInsertP(multimapP *m, const databox *elements[]);

/* Insert only if key doesn't exist */
bool multimapInsertIfNotExistsP(multimapP *m, const databox *elements[]);

/* Delete by key - returns true if found and deleted */
bool multimapDeleteP(multimapP *m, const databox *key);

/* Delete full-width entry (for sets) */
bool multimapDeleteFullWidthP(multimapP *m, const databox *elements[]);

/* Reset (clear all entries) */
void multimapResetP(multimapP *m);

/* Increment field value atomically */
int64_t multimapFieldIncrP(multimapP *m, const databox *key,
                           uint32_t fieldOffset, int64_t incrBy);

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
