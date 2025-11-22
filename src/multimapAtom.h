#pragma once

#include "mflex.h"
#include "multimap.h"

typedef struct multimapAtom multimapAtom;

typedef struct multimapAtomResult {
    databox val;
    uint64_t refcount;
} multimapAtomResult;

void multimapAtomInit(multimapAtom *ma);
multimapAtom *multimapAtomNew(void);
multimapAtom *multimapAtomNewCompress(mflexState *state);
void multimapAtomFree(multimapAtom *ma);

void multimapAtomInsert(multimapAtom *ma, const databox *box);
void multimapAtomInsertConvert(multimapAtom *ma, databox *box);
bool multimapAtomInsertIfNewConvert(multimapAtom *ma, databox *key);
bool multimapAtomInsertIfNewConvertAndRetain(multimapAtom *ma, databox *key);

void multimapAtomInsertWithExactAtomID(multimapAtom *ma, const uint64_t atomRef,
                                       const databox *key);

bool multimapAtomLookup(const multimapAtom *ma, const databox *ref,
                        databox *key);
void multimapAtomLookupConvert(const multimapAtom *ma, databox *box);
void multimapAtomLookupResult(const multimapAtom *ma, const databox *key,
                              multimapAtomResult *result);
void multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count);
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef);

bool multimapAtomLookupMin(const multimapAtom *ma, databox *minRef);

void multimapAtomRetain(multimapAtom *ma, const databox *key);
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef);
void multimapAtomRetainById(multimapAtom *ma, uint64_t id);

bool multimapAtomRelease(multimapAtom *ma, const databox *key);
bool multimapAtomReleaseById(multimapAtom *ma, const databox *foundRef);
bool multimapAtomDelete(multimapAtom *ma, const databox *key);

bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref);
bool multimapAtomDeleteById(multimapAtom *ma, const uint64_t id);

size_t multimapAtomCount(const multimapAtom *ma);
size_t multimapAtomBytes(const multimapAtom *ma);

#ifdef DATAKIT_TEST
void multimapAtomRepr(const multimapAtom *ma);
int multimapAtomTest(int argc, char *argv[]);
#endif
