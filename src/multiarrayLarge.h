#pragma once

#include "dkTier.h"
#include "multiarrayMedium.h"
#include <stdint.h>

typedef struct multiarrayLarge multiarrayLarge;

multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax);
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium);
void multiarrayLargeFreeInside(multiarrayLarge *e);
void multiarrayLargeFree(multiarrayLarge *e);

struct multiarrayLargeNode;

/* Indexes as for multiarrayMedium: Insert up to the count, Get and the
 * deletes on elements that exist (Get's negative idx from the end); out of
 * range, Get answers NULL and a change is refused, reporting
 * DK_DIAG_MULTIARRAY_INDEX.  GetHead / GetTail answer NULL when empty. */

void multiarrayLargeInsert(multiarrayLarge *mar, const int32_t idx,
                           const void *s);
void multiarrayLargeDelete(multiarrayLarge *mar, const int32_t idx);
void multiarrayLargeDeleteTaking(multiarrayLarge *mar, const int32_t idx,
                                 void *out);
void multiarrayLargeDeleteRange(multiarrayLarge *mar, const uint32_t idx,
                                uint32_t count);
/* The node holding entry 'idx', the node before it and the offset in it */
void multiarrayLargeLocate(const multiarrayLarge *mar, const uint32_t idx,
                           struct multiarrayLargeNode **prev,
                           struct multiarrayLargeNode **node, uint32_t *offset);
void *multiarrayLargeGet(const multiarrayLarge *mar, const int32_t idx);
size_t multiarrayLargeCount(const multiarrayLarge *mar);
size_t multiarrayLargeBytes(const multiarrayLarge *mar);
void multiarrayLargeStats(const multiarrayLarge *mar, dkTierStats *out);
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
