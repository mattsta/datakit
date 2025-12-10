#pragma once

#include "multiOrderedSet.h"
#include "multiOrderedSetCommon.h"

/* Forward declaration */
typedef struct multiOrderedSetSmall multiOrderedSetSmall;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */
multiOrderedSetSmall *multiOrderedSetSmallNew(void);
multiOrderedSetSmall *multiOrderedSetSmallCopy(const multiOrderedSetSmall *m);
void multiOrderedSetSmallFree(multiOrderedSetSmall *m);
void multiOrderedSetSmallReset(multiOrderedSetSmall *m);

/* ====================================================================
 * Statistics
 * ==================================================================== */
size_t multiOrderedSetSmallCount(const multiOrderedSetSmall *m);
size_t multiOrderedSetSmallBytes(const multiOrderedSetSmall *m);

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */
bool multiOrderedSetSmallAdd(multiOrderedSetSmall *m, const databox *score,
                             const databox *member);
bool multiOrderedSetSmallAddNX(multiOrderedSetSmall *m, const databox *score,
                               const databox *member);
bool multiOrderedSetSmallAddXX(multiOrderedSetSmall *m, const databox *score,
                               const databox *member);
bool multiOrderedSetSmallAddGetPrevious(multiOrderedSetSmall *m,
                                        const databox *score,
                                        const databox *member,
                                        databox *prevScore);
bool multiOrderedSetSmallIncrBy(multiOrderedSetSmall *m, const databox *delta,
                                const databox *member, databox *result);

/* ====================================================================
 * Deletion
 * ==================================================================== */
bool multiOrderedSetSmallRemove(multiOrderedSetSmall *m, const databox *member);
bool multiOrderedSetSmallRemoveGetScore(multiOrderedSetSmall *m,
                                        const databox *member, databox *score);
size_t multiOrderedSetSmallRemoveRangeByScore(multiOrderedSetSmall *m,
                                              const mosRangeSpec *range);
size_t multiOrderedSetSmallRemoveRangeByRank(multiOrderedSetSmall *m,
                                             int64_t start, int64_t stop);
size_t multiOrderedSetSmallPopMin(multiOrderedSetSmall *m, size_t count,
                                  databox *members, databox *scores);
size_t multiOrderedSetSmallPopMax(multiOrderedSetSmall *m, size_t count,
                                  databox *members, databox *scores);

/* ====================================================================
 * Lookup
 * ==================================================================== */
bool multiOrderedSetSmallExists(const multiOrderedSetSmall *m,
                                const databox *member);
bool multiOrderedSetSmallGetScore(const multiOrderedSetSmall *m,
                                  const databox *member, databox *score);
int64_t multiOrderedSetSmallGetRank(const multiOrderedSetSmall *m,
                                    const databox *member);
int64_t multiOrderedSetSmallGetReverseRank(const multiOrderedSetSmall *m,
                                           const databox *member);
bool multiOrderedSetSmallGetByRank(const multiOrderedSetSmall *m, int64_t rank,
                                   databox *member, databox *score);

/* ====================================================================
 * Range Queries
 * ==================================================================== */
size_t multiOrderedSetSmallCountByScore(const multiOrderedSetSmall *m,
                                        const mosRangeSpec *range);

/* ====================================================================
 * Iteration
 * ==================================================================== */
void multiOrderedSetSmallIteratorInit(const multiOrderedSetSmall *m,
                                      mosIterator *iter, bool forward);
bool multiOrderedSetSmallIteratorInitAtScore(const multiOrderedSetSmall *m,
                                             mosIterator *iter,
                                             const databox *score,
                                             bool forward);
bool multiOrderedSetSmallIteratorInitAtRank(const multiOrderedSetSmall *m,
                                            mosIterator *iter, int64_t rank,
                                            bool forward);
bool multiOrderedSetSmallIteratorNext(mosIterator *iter, databox *member,
                                      databox *score);

/* ====================================================================
 * First / Last
 * ==================================================================== */
bool multiOrderedSetSmallFirst(const multiOrderedSetSmall *m, databox *member,
                               databox *score);
bool multiOrderedSetSmallLast(const multiOrderedSetSmall *m, databox *member,
                              databox *score);

/* ====================================================================
 * Random
 * ==================================================================== */
size_t multiOrderedSetSmallRandomMembers(const multiOrderedSetSmall *m,
                                         int64_t count, databox *members,
                                         databox *scores);

/* ====================================================================
 * Debugging
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multiOrderedSetSmallRepr(const multiOrderedSetSmall *m);
int multiOrderedSetSmallTest(int argc, char *argv[]);
#endif
