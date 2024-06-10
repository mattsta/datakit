#pragma once

#include "multiarraySmallInternal.h"

/* We typedef'd multiarraySmallNode to multiarrayMediumNode because
 * all we need is 'data' and 'count' and small has them both already. */
typedef multiarraySmall multiarrayMediumNode;

/* This looks curiously like our pre-existing multiarraySmall except
 * we have an explicit 'data'->'node' here. */
/* multiarrayMedium is a 16 byte struct.
 * 'node' is an array of multiarraySmall containers.
 * 'count' is the number of multiarraySmall containers.
 * 'len' is the width of each entry inside each multiarraySmall container.
 * 'rowMax' is the maximum number of entires inside each multiarraySmall
 *          container before we grow 'node' to use a new multiarraySmall
 *          container for new data. */
struct multiarrayMedium {
    multiarrayMediumNode *node;
    uint32_t count;  /* number of 'node' (i.e. node is size: len * count) */
    uint16_t len;    /* width of individual entries in node->data */
    uint16_t rowMax; /* maximum entires inside each 'node' */
};
