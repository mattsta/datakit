#pragma once

#include "multimapCommon.h"

/* Opaque data type; no user visible data here */
typedef struct multimapSmall multimapSmall;

multimapSmall *multimapSmallNew(multimapElements elementsPerEntry,
                                bool mapIsSet);
multimapSmall *multimapSmallCopy(const multimapSmall *m);
size_t multimapSmallCount(const multimapSmall *m);
size_t multimapSmallBytes(const multimapSmall *m);
flex *multimapSmallDump(const multimapSmall *m);

bool multimapSmallInsert(multimapSmall *m, const databox *elements[]);
/* multimapSmallInsert that also reports the replaced value entries
 * ('elementsPerEntry - 1' slots, VOID when nothing was replaced; see
 * flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing).  Integers
 * up to 64 bits, reals and pointers stay valid; a BYTES or container slot
 * must not be read.  A replaced 96/128-bit integer reports only its type
 * (SIGNED_128 or UNSIGNED_128) with a NULL payload pointer. */
bool multimapSmallInsertReplacing(multimapSmall *m, const databox *elements[],
                                  databox *replacedOut);
void multimapSmallInsertFullWidth(multimapSmall *m, const databox *elements[]);
void multimapSmallAppend(multimapSmall *m, const databox *elements[]);

void multimapSmallInsertWithSurrogateKey(
    multimapSmall *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapSmallExists(multimapSmall *m, const databox *key);
bool multimapSmallExistsFullWidth(multimapSmall *m, const databox *elements[]);
bool multimapSmallExistsWithReference(
    multimapSmall *m, const databox *key, databox *foundRef,
    const struct multimapAtom *referenceContainer);

bool multimapSmallLookup(multimapSmall *m, const databox *key,
                         databox *elements[]);
/* fromTail == false: uniform random row; true: the last row, every call.
 * See multimapFull.h. */
bool multimapSmallRandomValue(multimapSmall *m, bool fromTail,
                              databox **foundBox, multimapEntry *me);
bool multimapSmallDeleteRandomValue(multimapSmall *m, bool deleteFromTail,
                                    databox **deletedBox);

bool multimapSmallGetUnderlyingEntry(multimapSmall *m, const databox *key,
                                     multimapEntry *me);
bool multimapSmallGetUnderlyingEntryWithReference(
    multimapSmall *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
void multimapSmallResizeEntry(multimapSmall *m, multimapEntry *me,
                              size_t newLen);
void multimapSmallReplaceEntry(multimapSmall *m, multimapEntry *me,
                               const databox *box);

bool multimapSmallDelete(multimapSmall *m, const databox *key);
bool multimapSmallDeleteFullWidth(multimapSmall *m, const databox *elements[]);
bool multimapSmallDeleteWithFound(multimapSmall *m, const databox *key,
                                  databox *foundReference);
bool multimapSmallDeleteWithReference(
    multimapSmall *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference);

int64_t multimapSmallFieldIncr(multimapSmall *m, const databox *key,
                               uint32_t fieldOffset, int64_t incrBy);

bool multimapSmallLookupByPosition(multimapSmall *m, int64_t keyIndex,
                                   databox *elements[]);
bool multimapSmallDeleteByPosition(multimapSmall *m, int64_t keyIndex);

void multimapSmallReset(multimapSmall *m);
void multimapSmallFree(multimapSmall *m);

bool multimapSmallFirst(multimapSmall *m, databox *elements[]);
bool multimapSmallLast(multimapSmall *m, databox *elements[]);

bool multimapSmallIteratorInitAt(multimapSmall *m, multimapIterator *iter,
                                 bool forward, const databox *startAt);
bool multimapSmallIteratorInit(multimapSmall *m, multimapIterator *iter,
                               bool forward);
bool multimapSmallIteratorNext(multimapIterator *iter, databox *elements[]);

bool multimapSmallDeleteByPredicate(multimapSmall *m,
                                    const multimapPredicate *p);

#ifdef DATAKIT_TEST
void multimapSmallRepr(const multimapSmall *m);
void multimapSmallVerify(const multimapSmall *m);
int multimapSmallTest(int argc, char *argv[]);
#endif
