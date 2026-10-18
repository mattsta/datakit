#pragma once

#include "mflex.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * prefix struct type.
 *
 * 'wide' is the iterator's storage for one 96/128-bit element:
 * multilistNext() decodes a wide element there, so the entry it fills
 * points into the iterator until the next multilistNext() on it.  Narrow
 * elements never touch it.  It is part of the struct, so nothing is
 * allocated, Release stays optional, and a struct copy of an iterator has
 * a slot of its own (a box read through the original still points into
 * the original). */
typedef struct multilistIterator {
    void *ml; /* pointer back to (untagged) multilist instance itself. */
    flexEntry *fe;
    /* offset of 'fe' (the element the next multilistNext returns) in the
     * current flex: head-relative when >= 0, tail-relative when < 0.
     * multilistDelEntry keeps it naming 'fe'. */
    int32_t offset;

    /* Medium and Full */
    mlNodeId nodeIdx; /* only used for Medium and Full */

    /* Full Only */
    flex *f;
    mflexState *state[2];
    bool readOnly;

    /* All */
    bool forward;
    uint32_t type; /* used for function dispatching */

    databoxWide wide; /* payload of the last wide element from Next */
} multilistIterator;  /* 80 bytes, 16-byte aligned */

/* multilistEntry is the result of multilistNext(), multilistIndex(),
 * multilistIndexNear() and their Wide forms.  The library sets ml, fe,
 * nodeIdx, offset and f (when the tier uses them) and decodes the element
 * into 'box' like flexGetByTypeInto(): it never reads the entry's prior
 * contents, and for a narrow element only box's type, data and (for
 * variable-length types) len are written, so read the type and the value,
 * not the 'big' and 'allocated' bits.
 *
 * A 96/128-bit element:
 *   - from multilistNext() has big = 1 and data.i128 / data.u128 aimed at
 *     the iterator's 'wide' slot;
 *   - from multilistIndexWide() / multilistIndexNearWide() has big = 1 and
 *     the pointer aimed at the caller's slot;
 *   - from multilistIndex() / multilistIndexNear() is deferred: type only,
 *     data.ptr = NULL, big = 0 (databoxIsWideDeferred).  Read it again
 *     with a Wide form when the value is needed.
 *
 * 'box' borrows:
 *   - BYTES and containers point into the list's flex (on the Full tier,
 *     possibly a node decompressed into the caller's mflexState), valid
 *     until the list is next modified or that state is reused;
 *   - a 96/128-bit value points into its slot, valid until the next
 *     multilistNext() on that iterator, or for as long as the caller's
 *     slot lives.
 * To keep a value past either, copy it with databoxCopy() (owned; release
 * with databoxFreeData()) or databoxBigCopyFrom() (re-homed into a
 * databoxBig, no allocation). */
typedef struct multilistEntry {
    void *ml; /* pointer back to (untagged) multilist instance itself. */
    flexEntry *fe;
    databox box;
    mlNodeId nodeIdx; /* only used for Medium and Full */
    /* offset of 'fe' in its flex: head-relative when >= 0, tail-relative
     * when < 0 (a reverse walk, or a lookup that walked from the tail) */
    int32_t offset;
    flex *f; /* Full only */
} multilistEntry;

_Static_assert(sizeof(multilistEntry) == 48, "multilistEntry must be 48 bytes");
_Static_assert(sizeof(multilistIterator) == 80,
               "multilistIterator must be 80 bytes");
_Static_assert(offsetof(multilistIterator, wide) % 16 == 0,
               "the iterator's wide slot must be 16-byte aligned");

typedef mflexState multilistState;

/* Internal to the tier DelEntry implementations: re-aim iter->offset at
 * iter->fe after the element at 'deletedOffset' (the entry Next just
 * returned) is deleted.  A delete moves the head-relative offsets (>= 0)
 * of the elements after it down by one, and the tail-relative offsets
 * (< 0) of the elements before it up by one.  A forward iterator's next
 * element is after the deleted one and a reverse iterator's is before
 * it, so only those two cases change. */
static inline void multilistIterOffsetAfterDelete_(multilistIterator *iter,
                                                   int32_t deletedOffset) {
    if (iter->forward) {
        if (deletedOffset >= 0) {
            iter->offset--;
        }
    } else if (deletedOffset < 0) {
        iter->offset++;
    }
}

/* Internal to the tier Pop implementations: the self-contained copy of
 * the element at 'fe', which is about to be deleted.  It is
 * flexGetByTypeCopy()'s result, except that BYTES of 8 bytes or less come
 * back embedded (DATABOX_BYTES_EMBED, allocated = 0), so popping a narrow
 * value never allocates.  Fixed-width values decode once; long BYTES,
 * containers and 96/128-bit values go through flexGetByTypeCopy(), which
 * writes a wide value in band when got->big is set and to a heap payload
 * otherwise. */
static inline void multilistPopCopy_(const flexEntry *fe, databox *got) {
    databox found = {{0}};
    flexGetByTypeInto(fe, &found, NULL);
    if (found.type == DATABOX_BYTES) {
        if (found.len <= sizeof(found.data.bytes.embed8)) {
            databox embed = {{0}};
            memcpy(embed.data.bytes.embed8, found.data.bytes.start, found.len);
            embed.type = DATABOX_BYTES_EMBED;
            embed.len = found.len;
            *got = embed;
            return;
        }
    } else if (found.type < DATABOX_BYTES && !databoxIsWideDeferred(&found)) {
        /* Nothing below BYTES borrows from the flex; flags are clear. */
        *got = found;
        return;
    }

    flexGetByTypeCopy(fe, got);
}
