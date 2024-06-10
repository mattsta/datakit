#pragma once

#include "mflex.h"
#include <stdint.h>

/* mlNodeId is the offset extent in our 'nodes' array.
 * Offsets can be forward (positive) or reverse (negative).
 * This limits us to 2 billion nodes.
 * Examples:
 *   - If each node holds 200 small entires (~40 bytes each),
 *     this limits us to 400 billion maximum entires.
 *   - If each node holds one entry (each entry > 8kb),
 *     this limits us to 2 billion maximum entries.
 *   - If each node holds 4,000 2 byte integers,
 *     this limuts us to 8 trillion maximum entires. */
typedef int32_t mlNodeId;

/* mlOffsetId is the offset extent in our entire list.
 * Offsets can be forward (positive) or reverse (negative).
 * This limits us to 9 quintillion maximum entires, but we
 * will hit physical node limits first. */
typedef int64_t mlOffsetId;

/* Note: this is oversized for small/medium because we stuff required Full state
 * in the common iterator too.
 * If space is a problem, we could refactor this futher into an extensible
 * prefix struct type. */
typedef struct multilistIterator {
    void *ml; /* pointer back to (untagged) multilist instance itself. */
    flexEntry *fe;
    int32_t offset; /* offset in current flex */

    /* Medium and Full */
    mlNodeId nodeIdx; /* only used for Medium and Full */

    /* Full Only */
    flex *f;
    mflexState *state[2];
    bool readOnly;

    /* All */
    bool forward;
    uint32_t type; /* used for function dispatching */
} multilistIterator;

/* multilistEntry is the result of either:
 *   multilistIndex() or multilistNext() calls
 * multilistEntry result data is held inside the databox 'box' */
typedef struct multilistEntry {
    void *ml; /* pointer back to (untagged) multilist instance itself. */
    flexEntry *fe;
    databox box;
    mlNodeId nodeIdx; /* only used for Medium and Full */
    int32_t offset;
    flex *f; /* Full only */
} multilistEntry;

typedef mflexState multilistState;
