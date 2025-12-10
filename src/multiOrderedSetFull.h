#pragma once

#include "atomPool.h"
#include "multiOrderedSet.h"
#include "multiOrderedSetCommon.h"

/* Forward declarations */
typedef struct multiOrderedSetFull multiOrderedSetFull;
struct multiOrderedSetMedium;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */
multiOrderedSetFull *multiOrderedSetFullNew(void);

/* Create with external atomPool for member interning (pool not owned).
 * When using a pool, scoreMap stores member IDs instead of member bytes,
 * significantly reducing memory for large datasets with long strings. */
multiOrderedSetFull *multiOrderedSetFullNewWithPool(atomPool *pool);

/* Create with owned atomPool using default HASH backend (fast O(1)) */
multiOrderedSetFull *multiOrderedSetFullNewWithOwnedPool(void);

/* Create with owned atomPool using specified backend type:
 * - ATOM_POOL_HASH: O(1) operations, higher memory (~84 bytes/entry)
 * - ATOM_POOL_TREE: O(log n) operations, lower memory (~22 bytes/entry) */
multiOrderedSetFull *multiOrderedSetFullNewWithPoolType(atomPoolType type);

multiOrderedSetFull *
multiOrderedSetFullNewFromMedium(struct multiOrderedSetMedium *medium,
                                 flex *maps[2], uint32_t middles[2]);
multiOrderedSetFull *multiOrderedSetFullCopy(const multiOrderedSetFull *m);
void multiOrderedSetFullFree(multiOrderedSetFull *m);
void multiOrderedSetFullReset(multiOrderedSetFull *m);

/* ====================================================================
 * Statistics
 * ==================================================================== */
size_t multiOrderedSetFullCount(const multiOrderedSetFull *m);
size_t multiOrderedSetFullBytes(const multiOrderedSetFull *m);

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */
bool multiOrderedSetFullAdd(multiOrderedSetFull *m, const databox *score,
                            const databox *member);
bool multiOrderedSetFullAddNX(multiOrderedSetFull *m, const databox *score,
                              const databox *member);
bool multiOrderedSetFullAddXX(multiOrderedSetFull *m, const databox *score,
                              const databox *member);
bool multiOrderedSetFullAddGetPrevious(multiOrderedSetFull *m,
                                       const databox *score,
                                       const databox *member,
                                       databox *prevScore);
bool multiOrderedSetFullIncrBy(multiOrderedSetFull *m, const databox *delta,
                               const databox *member, databox *result);

/* ====================================================================
 * Deletion
 * ==================================================================== */
bool multiOrderedSetFullRemove(multiOrderedSetFull *m, const databox *member);
bool multiOrderedSetFullRemoveGetScore(multiOrderedSetFull *m,
                                       const databox *member, databox *score);
size_t multiOrderedSetFullRemoveRangeByScore(multiOrderedSetFull *m,
                                             const mosRangeSpec *range);
size_t multiOrderedSetFullRemoveRangeByRank(multiOrderedSetFull *m,
                                            int64_t start, int64_t stop);
size_t multiOrderedSetFullPopMin(multiOrderedSetFull *m, size_t count,
                                 databox *members, databox *scores);
size_t multiOrderedSetFullPopMax(multiOrderedSetFull *m, size_t count,
                                 databox *members, databox *scores);

/* ====================================================================
 * Lookup
 * ==================================================================== */
bool multiOrderedSetFullExists(const multiOrderedSetFull *m,
                               const databox *member);
bool multiOrderedSetFullGetScore(const multiOrderedSetFull *m,
                                 const databox *member, databox *score);
int64_t multiOrderedSetFullGetRank(const multiOrderedSetFull *m,
                                   const databox *member);
int64_t multiOrderedSetFullGetReverseRank(const multiOrderedSetFull *m,
                                          const databox *member);
bool multiOrderedSetFullGetByRank(const multiOrderedSetFull *m, int64_t rank,
                                  databox *member, databox *score);

/* ====================================================================
 * Range Queries
 * ==================================================================== */
size_t multiOrderedSetFullCountByScore(const multiOrderedSetFull *m,
                                       const mosRangeSpec *range);

/* ====================================================================
 * Iteration
 * ==================================================================== */
void multiOrderedSetFullIteratorInit(const multiOrderedSetFull *m,
                                     mosIterator *iter, bool forward);
bool multiOrderedSetFullIteratorInitAtScore(const multiOrderedSetFull *m,
                                            mosIterator *iter,
                                            const databox *score, bool forward);
bool multiOrderedSetFullIteratorInitAtRank(const multiOrderedSetFull *m,
                                           mosIterator *iter, int64_t rank,
                                           bool forward);
bool multiOrderedSetFullIteratorNext(mosIterator *iter, databox *member,
                                     databox *score);

/* ====================================================================
 * First / Last
 * ==================================================================== */
bool multiOrderedSetFullFirst(const multiOrderedSetFull *m, databox *member,
                              databox *score);
bool multiOrderedSetFullLast(const multiOrderedSetFull *m, databox *member,
                             databox *score);

/* ====================================================================
 * Random
 * ==================================================================== */
size_t multiOrderedSetFullRandomMembers(const multiOrderedSetFull *m,
                                        int64_t count, databox *members,
                                        databox *scores);

/* ====================================================================
 * Debugging
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multiOrderedSetFullRepr(const multiOrderedSetFull *m);
int multiOrderedSetFullTest(int argc, char *argv[]);
#endif
