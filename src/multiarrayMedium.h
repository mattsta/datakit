#pragma once

#include "multiarraySmall.h"

typedef struct multiarrayMedium multiarrayMedium;

multiarrayMedium *multiarrayMediumNew(int16_t len, int16_t rowMax);
multiarrayMedium *multiarrayMediumFromSmall(multiarraySmall *small);
multiarrayMedium *multiarrayMediumNewWithData(int16_t len, int16_t rowMax,
                                              uint16_t count, void *data);

void multiarrayMediumFreeInside(multiarrayMedium *e);
void multiarrayMediumFree(multiarrayMedium *e);

/* multiarrayMediumInsert{Before,After} provided by multiarrayMediumMedium.c */
/* need: delete-at-idx */
/* need: delete-at-idx for count */
/* need: index-by-head */
/* need: index-by-tail */

void multiarrayMediumInsert(multiarrayMedium *mar, const int32_t idx,
                            const void *s);
void multiarrayMediumDelete(multiarrayMedium *mar, const int32_t idx);
void *multiarrayMediumGet(const multiarrayMedium *mar, const int32_t idx);
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
