#pragma once

#include "datakit.h"

typedef struct ptrPrevNext ptrPrevNext;
typedef size_t ptrPrevNextPtr;

ptrPrevNext *ptrPrevNextNew(void);
void ptrPrevNextFree(ptrPrevNext *ppn);

size_t ptrPrevNextAdd(ptrPrevNext *ppn, const size_t atomIndex,
                      const size_t prevOffset, const size_t nextOffset);
void ptrPrevNextRead(const ptrPrevNext *ppn, const ptrPrevNextPtr memOffset,
                     size_t *atomRef, size_t *prev, size_t *next);
void ptrPrevNextRelease(ptrPrevNext *ppn, const size_t memOffset);

ptrPrevNextPtr ptrPrevNextUpdate(ptrPrevNext *ppn,
                                 const ptrPrevNextPtr memOffset,
                                 const size_t prevOffset,
                                 const size_t nextOffset);

#ifdef DATAKIT_TEST
void ptrPrevNextRepr(ptrPrevNext *ppn);
int ptrPrevNextTest(int argc, char *argv[]);
#endif
