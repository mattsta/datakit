/* multiOrderedSetP.h - Persistent multiOrderedSet wrapper
 *
 * Provides automatic WAL logging for all multiOrderedSet mutations.
 * All operations mirror the standard multiOrderedSet API with 'P' suffix.
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/data/myset", NULL);
 *   multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
 *
 *   // Operations are automatically persisted
 *   databox score, member;
 *   databoxSetInt64(&score, 100);
 *   databoxSetBytes(&member, "alice", 5);
 *   multiOrderedSetAddP(mos, &score, &member);
 *
 *   // Clean shutdown
 *   multiOrderedSetCloseP(mos);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../multiOrderedSet.h"
#include "persistCtx.h"

/* ============================================================================
 * Types
 * ============================================================================
 */

typedef struct multiOrderedSetP {
    multiOrderedSet *mos; /* Underlying multiOrderedSet */
    persistCtx *ctx;      /* Persistence context */
} multiOrderedSetP;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create new persistent multiOrderedSet */
multiOrderedSetP *multiOrderedSetNewP(persistCtx *ctx);

/* Open existing persistent multiOrderedSet (recover from files) */
multiOrderedSetP *multiOrderedSetOpenP(persistCtx *ctx);

/* Close persistent multiOrderedSet (syncs and frees) */
void multiOrderedSetCloseP(multiOrderedSetP *mos);

/* Get underlying multiOrderedSet (for read-only operations) */
multiOrderedSet *multiOrderedSetGetRawP(multiOrderedSetP *mos);

/* ============================================================================
 * Metadata (read-only, no persistence needed)
 * ============================================================================
 */

size_t multiOrderedSetCountP(const multiOrderedSetP *mos);
size_t multiOrderedSetBytesP(const multiOrderedSetP *mos);

/* ============================================================================
 * Mutations (automatically persisted)
 * ============================================================================
 */

/* Add (score, member) pair - updates score if member exists */
bool multiOrderedSetAddP(multiOrderedSetP *mos, const databox *score,
                         const databox *member);

/* Remove member by name */
bool multiOrderedSetRemoveP(multiOrderedSetP *mos, const databox *member);

/* Remove all entries */
void multiOrderedSetResetP(multiOrderedSetP *mos);

/* ============================================================================
 * Lookups (read-only, no persistence needed)
 * ============================================================================
 */

/* Check if member exists */
bool multiOrderedSetExistsP(const multiOrderedSetP *mos, const databox *member);

/* Get score for member */
bool multiOrderedSetGetScoreP(const multiOrderedSetP *mos,
                              const databox *member, databox *score);

/* Get entry by rank (0-based index in sorted order) */
bool multiOrderedSetGetByRankP(const multiOrderedSetP *mos, int64_t rank,
                               databox *member, databox *score);

/* Get first/last entries */
bool multiOrderedSetFirstP(const multiOrderedSetP *mos, databox *member,
                           databox *score);
bool multiOrderedSetLastP(const multiOrderedSetP *mos, databox *member,
                          databox *score);

/* Count entries in score range */
size_t multiOrderedSetCountByScoreP(const multiOrderedSetP *mos,
                                    const mosRangeSpec *range);

/* ============================================================================
 * Iteration (read-only, no persistence needed)
 * ============================================================================
 */

/* Initialize iterator (forward or reverse) */
void multiOrderedSetIteratorInitP(const multiOrderedSetP *mos,
                                  mosIterator *iter, bool forward);

/* Initialize iterator at specific score */
bool multiOrderedSetIteratorInitAtScoreP(const multiOrderedSetP *mos,
                                         mosIterator *iter,
                                         const databox *score, bool forward);

/* Get next entry from iterator */
bool multiOrderedSetIteratorNextP(mosIterator *iter, databox *member,
                                  databox *score);

/* Release iterator resources */
void multiOrderedSetIteratorReleaseP(mosIterator *iter);

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

/* Force sync to disk */
bool multiOrderedSetSyncP(multiOrderedSetP *mos);

/* Force compaction now */
bool multiOrderedSetCompactP(multiOrderedSetP *mos);

/* Get persistence statistics */
void multiOrderedSetGetStatsP(const multiOrderedSetP *mos,
                              persistCtxStats *stats);

#ifdef DATAKIT_TEST
int multiOrderedSetPTest(int argc, char *argv[]);
#endif
