#pragma once

#include "multimapCommon.h"

/* Reference Container */
#include "multimapAtom.h"

/* (8 * 2) + (4 * 2) + 4 = 28 bytes of fields, padded to 32 */
struct multimapMedium {
    flex *map[2];       /* maps stored in low->high order */
    uint32_t middle[2]; /* offsets to middle of sorted maps */
    multimapElements elementsPerEntry : 16; /* max 64k "columns" per row */
    multimapElements spare : 1;             /* free flag */
    multimapElements mapIsSet : 1;    /* bool; set mode dedupes WHOLE rows */
    multimapElements isSurrogate : 1; /* bool; true if all keys need refs */
    multimapElements unused : 13;     /* free flags! */
};
