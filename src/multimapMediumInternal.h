#pragma once

#include "multimapCommon.h"

/* Reference Container */
#include "multimapAtom.h"

/*  8 + 8 + 4 + 4 + 4 = 28 bytes */
struct multimapMedium {
    flex *map[2];       /* maps stored in low->high order */
    uint32_t middle[2]; /* offsets to middle of sorted maps */
    multimapElements elementsPerEntry : 16; /* max 64k "columns" per row */
    multimapElements compress : 1;    /* bool; true if compression enabled */
    multimapElements mapIsSet : 1;    /* bool; true if keys must be unique. */
    multimapElements isSurrogate : 1; /* bool; true if all keys need refs */
    multimapElements unused : 13;     /* free flags! */
};
