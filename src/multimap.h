#pragma once

#include "flexCapacityManagement.h"
#include "multimapCommon.h"

/* opaque multimap type; there's no user accessible data here. */
typedef struct multimap multimap;

multimap *multimapNew(multimapElements elementsPerEntry);
multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const uint32_t limit);
multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const uint32_t limit);
multimap *multimapNewConfigure(multimapElements elementsPerEntry, bool isSet,
                               bool compress, flexCapSizeLimit sizeLimit);
multimap *multimapSetNew(multimapElements elementsPerEntry);
multimap *multimapCopy(const multimap *m);
size_t multimapCount(const multimap *m);
size_t multimapBytes(const multimap *m);
flex *multimapDump(const multimap *m);

bool multimapInsert(multimap **m, const databox *elements[]);
void multimapInsertFullWidth(multimap **m, const databox *elements[]);
void multimapAppend(multimap **m, const databox *elements[]);

void multimapInsertWithSurrogateKey(
    multimap **m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me);
bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
void multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen);
void multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box);

bool multimapExists(const multimap *m, const databox *key);
bool multimapExistsFullWidth(const multimap *m, const databox *elements[]);
bool multimapExistsWithReference(const multimap *m, const databox *key,
                                 databox *foundRef,
                                 const struct multimapAtom *referenceContainer);

bool multimapLookup(const multimap *m, const databox *key, databox *elements[]);
bool multimapRandomValue(multimap *m, bool fromTail, databox **found,
                         multimapEntry *me);

bool multimapDelete(multimap **m, const databox *key);
bool multimapDeleteFullWidth(multimap **m, const databox *elements[]);
bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference);
bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference);
bool multimapDeleteRandomValue(multimap **m, bool fromTail, databox **deleted);

int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy);

void multimapReset(multimap *m);
void multimapFree(multimap *m);

void multimapEntryResize(multimap **m, const databox *key, size_t newSize);
void multimapEntryReplace(multimap **m, const databox *key,
                          const databox *elements[]);

bool multimapFirst(multimap *m, databox *elements[]);
bool multimapLast(multimap *m, databox *elements[]);

/* ADD THESE */
bool multimapDeleteByPosition(multimap *m, int64_t keyIndex);
bool multimapLookupByPosition(multimap *m, int64_t keyIndex,
                              databox *elements[]);

bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p);
bool multimapProcessPredicate(const multimapPredicate *p, const databox *value);
typedef bool(multimapElementWalker)(void *userData, const databox *elements[]);
size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData);

bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            bool forward, const databox *box);
void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          bool forward);
bool multimapIteratorNext(multimapIterator *iter, databox **elements);

void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b);

void multimapDifferenceKeys(multimap **restrict dst,
                            multimapIterator *restrict const a,
                            multimapIterator *restrict const b,
                            const bool symmetricDifference);

void multimapCopyKeys(multimap **restrict dst, const multimap *restrict src);

#ifdef DATAKIT_TEST
void multimapRepr(const multimap *m);
int multimapTest(int argc, char *argv[]);
#endif
