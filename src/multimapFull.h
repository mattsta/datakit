#pragma once

#include "databox.h"
#include "multimapCommon.h"

/* Opaque data type; no user visible data here */
typedef struct multimapFull multimapFull;
typedef void(multimapFullMapDeleter)(const flex *map, void *data);

multimapFull *multimapFullNew(multimapElements elementsPerEntry);
multimapFull *multimapFullSetNew(multimapElements elementsPerEntry,
                                 uint16_t maxSize);
multimapFull *multimapFullNewFromManyGrow(void *m, flex *map[],
                                          const multimapFullMiddle middle[],
                                          size_t count,
                                          multimapElements elementsPerEntry,
                                          bool mapIsSet);
multimapFull *multimapFullNewFromTwoGrow(void *m, flex *map[2],
                                         multimapFullMiddle middle[2],
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet);
multimapFull *multimapFullNewFromTwoGrowWithReference(
    void *m, flex *map[2], multimapFullMiddle middle[2],
    multimapElements elementsPerEntry, bool mapIsSet,
    const struct multimapAtom *referenceContainer);
multimapFull *multimapFullNewFromOneGrow(void *m, flex *one,
                                         multimapElements mid,
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet);
multimapFull *multimapFullCopy(const multimapFull *m);
size_t multimapFullCount(const multimapFull *m);
size_t multimapFullBytes(const multimapFull *m);
size_t multimapFullNodeCount(const multimapFull *m);
size_t multimapFullBytesFull(const multimapFull *m);
flex *multimapFullDump(const multimapFull *m);

bool multimapFullInsert(multimapFull *m, const databox *elements[]);
void multimapFullInsertFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullInsertAllowExternalizeKeys(multimapFull *m,
                                            const databox *elements[],
                                            void **keyAllocation);
void multimapFullAppend(multimapFull *m, const databox *elements[]);

bool multimapFullInsertWithSurrogateKey(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapFullExists(multimapFull *m, const databox *key);
bool multimapFullExistsFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullExistsWithReference(
    const multimapFull *m, const databox *key, databox *foundRef,
    const struct multimapAtom *referenceContainer);

bool multimapFullLookup(multimapFull *m, const databox *key,
                        databox *elements[]);
bool multimapFullRandomValue(multimapFull *m, const bool fromTail,
                             databox **foundBox, multimapEntry *me);
bool multimapFullGetUnderlyingEntry(multimapFull *m, const databox *key,
                                    multimapEntry *me);
bool multimapFullGetUnderlyingEntryWithReference(
    multimapFull *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
void multimapFullResizeEntry(multimapFull *m, multimapEntry *me, size_t newLen);
void multimapFullReplaceEntry(multimapFull *m, multimapEntry *me,
                              const databox *box);
void multimapFullDeleteEntry(multimapFull *m, multimapEntry *me);

bool multimapFullRegularizeMap(multimapFull *m, multimapFullIdx mapIdx,
                               flex **map);
bool multimapFullRegularizeMapWithReference(
    multimapFull *m, multimapFullIdx mapIdx, flex **map,
    const struct multimapAtom *referenceContainer);

bool multimapFullDelete(multimapFull *m, const databox *key);
bool multimapFullDeleteNMaps(multimapFull *m, size_t n);
bool multimapFullDeleteNMapsIterate(multimapFull *m, const size_t n,
                                    multimapFullMapDeleter *mapIter,
                                    void *data);
bool multimapFullDeleteFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullDeleteFullWidthWithFound(multimapFull *m,
                                          const databox *elements[],
                                          databox *found);
bool multimapFullDeleteWithReference(
    multimapFull *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference);
bool multimapFullDeleteWithFound(multimapFull *m, const databox *key,
                                 databox *foundReference);
bool multimapFullDeleteRandomValue(multimapFull *m, const bool deleteFromTail,
                                   databox **deletedBox);

int64_t multimapFullFieldIncr(multimapFull *m, const databox *key,
                              uint32_t fieldOffset, int64_t incrBy);

void multimapFullReset(multimapFull *m);
void multimapFullFree(multimapFull *m);

bool multimapFullFirst(multimapFull *m, databox *elements[]);
bool multimapFullLast(multimapFull *m, databox *elements[]);

bool multimapFullIteratorInitAt(multimapFull *m, multimapIterator *iter,
                                bool forward, const databox *box);
bool multimapFullIteratorInit(multimapFull *m, multimapIterator *iter,
                              bool forward);
bool multimapFullIteratorNext(multimapIterator *iter, databox *elements[]);

bool multimapFullDeleteByPredicate(multimapFull *m, const multimapPredicate *p);

#ifdef DATAKIT_TEST
void multimapFullVerify(const multimapFull *m);
void multimapFullRepr(const multimapFull *m);
int multimapFullTest(int argc, char *argv[]);
#endif
