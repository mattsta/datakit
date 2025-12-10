#pragma once

#include "multiOrderedSet.h"
#include "multiOrderedSetCommon.h"

/* Forward declaration */
typedef struct multiOrderedSetMedium multiOrderedSetMedium;
struct multiOrderedSetSmall;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */
multiOrderedSetMedium *multiOrderedSetMediumNew(void);
multiOrderedSetMedium *
multiOrderedSetMediumNewFromSmall(struct multiOrderedSetSmall *small, flex *map,
                                  uint32_t middle);
multiOrderedSetMedium *
multiOrderedSetMediumCopy(const multiOrderedSetMedium *m);
void multiOrderedSetMediumFree(multiOrderedSetMedium *m);
void multiOrderedSetMediumReset(multiOrderedSetMedium *m);

/* ====================================================================
 * Statistics
 * ==================================================================== */
size_t multiOrderedSetMediumCount(const multiOrderedSetMedium *m);
size_t multiOrderedSetMediumBytes(const multiOrderedSetMedium *m);

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */
bool multiOrderedSetMediumAdd(multiOrderedSetMedium *m, const databox *score,
                              const databox *member);
bool multiOrderedSetMediumAddNX(multiOrderedSetMedium *m, const databox *score,
                                const databox *member);
bool multiOrderedSetMediumAddXX(multiOrderedSetMedium *m, const databox *score,
                                const databox *member);
bool multiOrderedSetMediumAddGetPrevious(multiOrderedSetMedium *m,
                                         const databox *score,
                                         const databox *member,
                                         databox *prevScore);
bool multiOrderedSetMediumIncrBy(multiOrderedSetMedium *m, const databox *delta,
                                 const databox *member, databox *result);

/* ====================================================================
 * Deletion
 * ==================================================================== */
bool multiOrderedSetMediumRemove(multiOrderedSetMedium *m,
                                 const databox *member);
bool multiOrderedSetMediumRemoveGetScore(multiOrderedSetMedium *m,
                                         const databox *member, databox *score);
size_t multiOrderedSetMediumRemoveRangeByScore(multiOrderedSetMedium *m,
                                               const mosRangeSpec *range);
size_t multiOrderedSetMediumRemoveRangeByRank(multiOrderedSetMedium *m,
                                              int64_t start, int64_t stop);
size_t multiOrderedSetMediumPopMin(multiOrderedSetMedium *m, size_t count,
                                   databox *members, databox *scores);
size_t multiOrderedSetMediumPopMax(multiOrderedSetMedium *m, size_t count,
                                   databox *members, databox *scores);

/* ====================================================================
 * Lookup
 * ==================================================================== */
bool multiOrderedSetMediumExists(const multiOrderedSetMedium *m,
                                 const databox *member);
bool multiOrderedSetMediumGetScore(const multiOrderedSetMedium *m,
                                   const databox *member, databox *score);
int64_t multiOrderedSetMediumGetRank(const multiOrderedSetMedium *m,
                                     const databox *member);
int64_t multiOrderedSetMediumGetReverseRank(const multiOrderedSetMedium *m,
                                            const databox *member);
bool multiOrderedSetMediumGetByRank(const multiOrderedSetMedium *m,
                                    int64_t rank, databox *member,
                                    databox *score);

/* ====================================================================
 * Range Queries
 * ==================================================================== */
size_t multiOrderedSetMediumCountByScore(const multiOrderedSetMedium *m,
                                         const mosRangeSpec *range);

/* ====================================================================
 * Iteration
 * ==================================================================== */
void multiOrderedSetMediumIteratorInit(const multiOrderedSetMedium *m,
                                       mosIterator *iter, bool forward);
bool multiOrderedSetMediumIteratorInitAtScore(const multiOrderedSetMedium *m,
                                              mosIterator *iter,
                                              const databox *score,
                                              bool forward);
bool multiOrderedSetMediumIteratorInitAtRank(const multiOrderedSetMedium *m,
                                             mosIterator *iter, int64_t rank,
                                             bool forward);
bool multiOrderedSetMediumIteratorNext(mosIterator *iter, databox *member,
                                       databox *score);

/* ====================================================================
 * First / Last
 * ==================================================================== */
bool multiOrderedSetMediumFirst(const multiOrderedSetMedium *m, databox *member,
                                databox *score);
bool multiOrderedSetMediumLast(const multiOrderedSetMedium *m, databox *member,
                               databox *score);

/* ====================================================================
 * Random
 * ==================================================================== */
size_t multiOrderedSetMediumRandomMembers(const multiOrderedSetMedium *m,
                                          int64_t count, databox *members,
                                          databox *scores);

/* ====================================================================
 * Debugging
 * ==================================================================== */
#ifdef DATAKIT_TEST
void multiOrderedSetMediumRepr(const multiOrderedSetMedium *m);
int multiOrderedSetMediumTest(int argc, char *argv[]);
#endif
