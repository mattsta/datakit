#pragma once

#include "multiarrayMedium.h"
#include <stdint.h>

typedef struct multiarrayLarge multiarrayLarge;

multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax);
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium);
void multiarrayLargeFreeInside(multiarrayLarge *e);
void multiarrayLargeFree(multiarrayLarge *e);

struct multiarrayLargeNode;

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
