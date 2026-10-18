#pragma once

#include "mflex.h"
#include <stddef.h>
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

/* multilistEntry is the result of multilistNext(), multilistIndex() and
 * multilistIndexNear().  The library fills every field, so the entry needs
 * no initialization: its prior contents are never read.
 *
 * The element is 'box'.  'boxBig' is the same storage seen as a
 * databoxBig, whose 'extra' holds a 96/128-bit payload; for such a value
 * box.data.i128 / box.data.u128 points at boxBig.extra and box.big is 1.
 * Every other value has box.big = 0.
 *
 * 'box' borrows:
 *   - BYTES and containers point into the list's flex (on the Full tier,
 *     possibly a node decompressed into the caller's mflexState), valid
 *     until the list is next modified or that state is reused;
 *   - a 96/128-bit value points into the entry itself, valid until the
 *     entry is reused or goes out of scope, so a plain struct copy of
 *     'box' still points back into this entry.
 * To keep a value past either, copy it with databoxCopy() (owned; release
 * with databoxFreeData()) or databoxBigCopyFrom() (re-homed into another
 * databoxBig, no allocation). */
typedef struct multilistEntry {
    void *ml; /* pointer back to (untagged) multilist instance itself. */
    flexEntry *fe;
    union {
        databox box;       /* the element */
        databoxBig boxBig; /* same bytes; 'extra' holds a wide payload */
    };
    mlNodeId nodeIdx; /* only used for Medium and Full */
    int32_t offset;
    flex *f;      /* Full only */
} multilistEntry; /* 64 bytes, 16-byte aligned */

_Static_assert(sizeof(multilistEntry) == 64,
               "multilistEntry must stay one 64-byte cache line");
_Static_assert(_Alignof(multilistEntry) == _Alignof(databoxBig),
               "multilistEntry must carry databoxBig's alignment");
_Static_assert(offsetof(multilistEntry, boxBig) % 16 == 0,
               "the wide payload in multilistEntry must be 16-byte aligned");

typedef mflexState multilistState;
