#pragma once

#include "datakit.h"

/* A compact arena of {atomIndex, prev, next} entries of any size_t values,
 * each stored in 2 to 9 bytes per field.  An entry is named by a
 * ptrPrevNextPtr, which stays valid (also across the arena's growth)
 * until that entry is updated to a new pointer or released.
 * @handle: fixed: its address holds from creation to free */
typedef struct ptrPrevNext ptrPrevNext;
typedef size_t ptrPrevNextPtr;

ptrPrevNext *ptrPrevNextNew(void);
void ptrPrevNextFree(ptrPrevNext *ppn);

/* Store a new entry; returns its pointer */
size_t ptrPrevNextAdd(ptrPrevNext *ppn, const size_t atomIndex,
                      const size_t prevOffset, const size_t nextOffset);
void ptrPrevNextRead(const ptrPrevNext *ppn, const ptrPrevNextPtr memOffset,
                     size_t *atomRef, size_t *prev, size_t *next);
/* Free a live entry; its pointer is then invalid */
void ptrPrevNextRelease(ptrPrevNext *ppn, const ptrPrevNextPtr memOffset);

/* Replace a live entry's prev and next.  Returns the entry's pointer: the
 * same one when the new values fit in place, else a new one (and the old
 * pointer is then invalid). */
ptrPrevNextPtr ptrPrevNextUpdate(ptrPrevNext *ppn,
                                 const ptrPrevNextPtr memOffset,
                                 const size_t prevOffset,
                                 const size_t nextOffset);

#ifdef DATAKIT_TEST
void ptrPrevNextRepr(ptrPrevNext *ppn);
int ptrPrevNextTest(int argc, char *argv[]);
#endif
