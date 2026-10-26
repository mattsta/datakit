#pragma once

#include "multimapCommon.h"

/* Opaque data type; no user visible data here */
typedef struct multimapMedium multimapMedium;

[[nodiscard]] multimapMedium *
multimapMediumNew(multimapElements elementsPerEntry);
[[nodiscard]] multimapMedium *
multimapMediumNewFromOneGrow(void *m, flex *one, uint32_t middle,
                             multimapElements elementsPerEntry, bool mapIsSet);
[[nodiscard]] multimapMedium *multimapMediumCopy(const multimapMedium *m);
size_t multimapMediumCount(const multimapMedium *m);
size_t multimapMediumBytes(const multimapMedium *m);
flex *multimapMediumDump(const multimapMedium *m);

bool multimapMediumInsert(multimapMedium *m, const databox *elements[]);
/* multimapMediumInsert that also reports the replaced value entries
 * ('elementsPerEntry - 1' slots, VOID when nothing was replaced).  Integers
 * up to 64 bits, reals and pointers stay valid; a BYTES or container slot
 * must not be read.  A replaced 96/128-bit integer reports only its type
 * (SIGNED_128 or UNSIGNED_128) with a NULL payload pointer. */
bool multimapMediumInsertReplacing(multimapMedium *m, const databox *elements[],
                                   databox *replacedOut);
/* As multimapInsertIfAbsentEntry (multimap.h); 'me' may be NULL */
bool multimapMediumInsertIfAbsent(multimapMedium *m, const databox *elements[],
                                  multimapEntry *me);
/* As multimapInsertWithSurrogateKeyIfAbsent (multimap.h) */
bool multimapMediumInsertWithSurrogateKeyIfAbsent(
    multimapMedium *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx);
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
/* As multimapGetUnderlyingEntryFullWidth (multimap.h) */
bool multimapMediumGetUnderlyingEntryFullWidth(multimapMedium *m,
                                               const databox *elements[],
                                               multimapEntry *me);
void multimapMediumDeleteEntryWithReference(
    multimapMedium *m, multimapEntry *me,
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
bool multimapMediumLookupWithReference(
    multimapMedium *m, const databox *key, databox *elements[],
    const struct multimapAtom *referenceContainer);

/* fromTail == false: uniform random row; true: the last row, every call.
 * See multimapFull.h. */
bool multimapMediumRandomValue(multimapMedium *m, const bool fromTail,
                               databox **foundBox, multimapEntry *me);
bool multimapMediumDeleteRandomValue(multimapMedium *m,
                                     const bool deleteFromTail,
                                     databox **deletedBox);

bool multimapMediumDelete(multimapMedium *m, const databox *key);
bool multimapMediumDeleteReporting(multimapMedium *m, const databox *key,
                                   databox *removedOut);
bool multimapMediumDeleteTaking(multimapMedium *m, const databox *key,
                                databox *takenOut);
bool multimapMediumDeleteFullWidth(multimapMedium *m,
                                   const databox *elements[]);
bool multimapMediumDeleteWithReference(
    multimapMedium *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference);
bool multimapMediumDeleteWithFound(multimapMedium *m, const databox *key,
                                   databox *foundReference);

int64_t multimapMediumFieldIncr(multimapMedium *m, const databox *key,
                                uint32_t fieldOffset, int64_t incrBy);

bool multimapMediumLookupByPosition(multimapMedium *m, int64_t keyIndex,
                                    databox *elements[]);
bool multimapMediumDeleteByPosition(multimapMedium *m, int64_t keyIndex);

void multimapMediumReset(multimapMedium *m);
void multimapMediumFree(multimapMedium *m);

bool multimapMediumFirst(multimapMedium *m, databox *elements[]);
bool multimapMediumLast(multimapMedium *m, databox *elements[]);

bool multimapMediumIteratorInitAt(multimapMedium *m, multimapIterator *iter,
                                  bool forward, const databox *box);
bool multimapMediumIteratorInit(multimapMedium *m, multimapIterator *iter,
                                bool forward);
bool multimapMediumIteratorNext(multimapIterator *iter, databox *elements[]);
/* See multimapIteratorNextWithSlots. */
bool multimapMediumIteratorNextWithSlots(multimapIterator *iter,
                                         databox *elements[],
                                         databoxWide *slots);

bool multimapMediumDeleteByPredicate(multimapMedium *m,
                                     const multimapPredicate *p);

#ifdef DATAKIT_TEST
void multimapMediumRepr(const multimapMedium *m);
void multimapMediumVerify(const multimapMedium *m);
int multimapMediumTest(int argc, char *argv[]);
#endif
