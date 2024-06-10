#pragma once

#include "multimapCommon.h"

/* Reference Container */
#include "multimapAtom.h"

/* 8 + 4 + 4 = 16 bytes */
struct multimapSmall {
    flex *map;                              /* single map */
    uint32_t middle;                        /* offset to middle of sorted map */
    multimapElements elementsPerEntry : 16; /* max 64k "columns" per row */
    multimapElements compress : 1;    /* bool; true if compression enabled */
    multimapElements mapIsSet : 1;    /* bool; true if keys must be unique. */
    multimapElements isSurrogate : 1; /* bool; true if all keys need refs */
    multimapElements unused : 13;     /* free flags! */
};

#if DK_C11
_Static_assert(sizeof(multimapSmall) == (sizeof(flex *) + sizeof(uint32_t) * 2),
               "multimapSmall struct is bigger than we think it should be!");
#endif
