#pragma once

#include "multiarraySmall.h"

/* @handle: fixed: its address holds from creation to free */
typedef struct multiarrayMedium multiarrayMedium;

multiarrayMedium *multiarrayMediumNew(int16_t len, int16_t rowMax);
multiarrayMedium *multiarrayMediumFromSmall(multiarraySmall *small);
multiarrayMedium *multiarrayMediumNewWithData(int16_t len, int16_t rowMax,
                                              uint16_t count, void *data);

void multiarrayMediumFreeInside(multiarrayMedium *e);
void multiarrayMediumFree(multiarrayMedium *e);

/* Indexes (as for multiarrayLarge): Insert takes idx up to the count (the
 * append position); Get and the deletes take elements that exist, Get's
 * negative idx counting from the end (-1 is the last).  Out of range, Get
 * answers NULL and a change is refused, the array unchanged; both report
 * DK_DIAG_MULTIARRAY_INDEX.  GetHead / GetTail answer NULL when empty. */

void multiarrayMediumInsert(multiarrayMedium *mar, const int32_t idx,
                            const void *s);
void multiarrayMediumDelete(multiarrayMedium *mar, const int32_t idx);
void multiarrayMediumDeleteTaking(multiarrayMedium *mar, const int32_t idx,
                                  void *out);
void multiarrayMediumDeleteRange(multiarrayMedium *mar, const uint32_t idx,
                                 uint32_t count);
void multiarrayMediumLocate(const multiarrayMedium *mar, const uint32_t idx,
                            uint32_t *nodeIdx, uint32_t *offset);
void *multiarrayMediumGet(const multiarrayMedium *mar, const int32_t idx);
size_t multiarrayMediumCount(const multiarrayMedium *mar);
size_t multiarrayMediumBytes(const multiarrayMedium *mar);
void *multiarrayMediumGetForward(const multiarrayMedium *mar,
                                 const uint32_t idx);
void *multiarrayMediumGetHead(const multiarrayMedium *mar);
void *multiarrayMediumGetTail(const multiarrayMedium *mar);

/* need: get-middle */
/* need: binary-search-inline? */

/* need: merge-on-insert?  merge-on-delete? */

#ifdef DATAKIT_TEST
int multiarrayMediumTest(int argc, char *argv[]);
#endif
