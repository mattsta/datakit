#pragma once

#include "multiarray.h"
#include "multimapCommon.h"

/* Reference Container */
#include "multimapAtom.h"

/* multimapFull is a 40 byte struct describing a multimapFull.
 * 'map' is a map for each tiny map. (count * sizeof(map))
 * 'middle' is an array of integer offsets into maps for midpoint memories.
 * 'count' is the total number of values across all map.
 * 'rangeBox' is [low] databox for each map.
 * 'maxSize' is the size in bytes before we split a map in half.
 * 'values' is the count of all key/value pairs across all maps.
 * 'elementsPerEntry' allows multiple (or not) values per key */
struct multimapFull {
    multiarray *map;           /* flex *; maps stored in low->high order */
    multiarray *middle;        /* multimapFullMiddle; middle offsets */
    multiarray *rangeBox;      /* rangeBox; [head] databoxes for each map */
    multimapFullIdx count;     /* total number of maps */
    multimapFullValues values; /* count of all "rows" in every map */
    multimapElements elementsPerEntry; /* max 4 billion "columns" per row. */
    uint32_t maxSize : 17;             /* max 65536 cutoff for splitting */
    uint32_t mapIsSet : 1;             /* bool; true if keys are unique */
    uint32_t compress : 1;             /* bool; true if compression enabled */
    uint32_t isSurrogate : 1;          /* bool; true if all keys need refs */
    uint32_t unused : 12; /* more free flags! tiny flags for everybody! */
};
