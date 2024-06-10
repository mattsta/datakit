#pragma once

#include "multimapCommon.h"

/* Opaque data type; no user visible data here */
typedef struct multimapMedium multimapMedium;

multimapMedium *multimapMediumNew(multimapElements elementsPerEntry);
multimapMedium *multimapMediumNewFromOneGrow(void *m, flex *one,
                                             uint32_t middle,
                                             multimapElements elementsPerEntry,
                                             bool mapIsSet);
multimapMedium *multimapMediumCopy(const multimapMedium *m);
size_t multimapMediumCount(const multimapMedium *m);
size_t multimapMediumBytes(const multimapMedium *m);
flex *multimapMediumDump(const multimapMedium *m);

bool multimapMediumInsert(multimapMedium *m, const databox *elements[]);
void multimapMediumInsertFullWidth(multimapMedium *m,
                                   const databox *elements[]);
void multimapMediumAppend(multimapMedium *m, const databox *elements[]);

void multimapMediumInsertWithSurrogateKey(
    multimapMedium *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapMediumGetUnderlyingEntry(multimapMedium *m, const databox *key,
                                      multimapEntry *me);
bool multimapMediumGetUnderlyingEntryWithReference(
    multimapMedium *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
void multimapMediumResizeEntry(multimapMedium *m, multimapEntry *me,
                               size_t newLen);
void multimapMediumReplaceEntry(multimapMedium *m, multimapEntry *me,
                                const databox *box);

bool multimapMediumExists(multimapMedium *m, const databox *key);
bool multimapMediumExistsFullWidth(multimapMedium *m,
                                   const databox *elements[]);
bool multimapMediumExistsWithReference(
    multimapMedium *m, const databox *key, databox *foundRef,
    const struct multimapAtom *referenceContainer);

bool multimapMediumLookup(multimapMedium *m, const databox *key,
                          databox *elements[]);

bool multimapMediumRandomValue(multimapMedium *m, const bool fromTail,
                               databox **foundBox, multimapEntry *me);
bool multimapMediumDeleteRandomValue(multimapMedium *m,
                                     const bool deleteFromTail,
                                     databox **deletedBox);

bool multimapMediumDelete(multimapMedium *m, const databox *key);
bool multimapMediumDeleteFullWidth(multimapMedium *m,
                                   const databox *elements[]);
bool multimapMediumDeleteWithReference(
    multimapMedium *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference);
bool multimapMediumDeleteWithFound(multimapMedium *m, const databox *key,
                                   databox *foundReference);

int64_t multimapMediumFieldIncr(multimapMedium *m, const databox *key,
                                uint32_t fieldOffset, int64_t incrBy);

void multimapMediumReset(multimapMedium *m);
void multimapMediumFree(multimapMedium *m);

bool multimapMediumFirst(multimapMedium *m, databox *elements[]);
bool multimapMediumLast(multimapMedium *m, databox *elements[]);

bool multimapMediumIteratorInitAt(multimapMedium *m, multimapIterator *iter,
                                  bool forward, const databox *box);
bool multimapMediumIteratorInit(multimapMedium *m, multimapIterator *iter,
                                bool forward);
bool multimapMediumIteratorNext(multimapIterator *iter, databox *elements[]);

bool multimapMediumDeleteByPredicate(multimapMedium *m,
                                     const multimapPredicate *p);

#ifdef DATAKIT_TEST
void multimapMediumRepr(const multimapMedium *m);
int multimapMediumTest(int argc, char *argv[]);
#endif
