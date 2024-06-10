#pragma once

#include "multiarrayMedium.h"
#include <stdint.h>

typedef struct multiarrayLarge multiarrayLarge;

multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax);
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium);
void multiarrayLargeFreeInside(multiarrayLarge *e);
void multiarrayLargeFree(multiarrayLarge *e);

/* multiarrayLargeInsert{,After} provided by multiarrayMediumLarge.c */
/* need: delete-at-idx */
/* need: delete-at-idx for count */
/* need: index-by-head */
/* need: index-by-tail */

void multiarrayLargeInsert(multiarrayLarge *mar, const int32_t idx,
                           const void *s);
void multiarrayLargeDelete(multiarrayLarge *mar, const int32_t idx);
void *multiarrayLargeGet(const multiarrayLarge *mar, const int32_t idx);
void *multiarrayLargeGetForward(const multiarrayLarge *mar,
                                const uint32_t index);
void *multiarrayLargeGetHead(const multiarrayLarge *mar);
void *multiarrayLargeGetTail(const multiarrayLarge *mar);

/* need: get-middle */
/* need: binary-search-inline? */

/* need: merge-on-insert?  merge-on-delete? */

#ifdef DATAKIT_TEST
int multiarrayLargeTest(int argc, char *argv[]);
#endif
