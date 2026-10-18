#pragma once

/* Helpers shared by the multimap tiers.  Not part of the public API. */

#include "multimapAtom.h"
#include "multimapCommon.h"

/* A set never overwrites a value: a set insert reports each of the
 * 'elementsPerEntry - 1' replacedOut slots as VOID.  NULL replacedOut is a
 * no-op. */
static inline void
multimapReplacedOutClear_(databox *const replacedOut,
                          const multimapElements elementsPerEntry) {
    if (!replacedOut) {
        return;
    }

    for (multimapElements i = 1; i < elementsPerEntry; i++) {
        replacedOut[i - 1] = (databox){.type = DATABOX_VOID};
    }
}

/* Decode element 'i' of a row for the tier iterators.  With NULL 'slots'
 * this is flexGetByType, so the caller's box keeps the in-band contract.
 * Otherwise a 96/128-bit value lands in slots[i] (flexGetByTypeInto) and
 * a narrow value never touches it.  The tier iterators inline this with
 * 'slots' known to be NULL or non-NULL, so the test folds away. */
DK_INLINE_ALWAYS void multimapIteratorDecode_(const flexEntry *const fe,
                                              databox *const box,
                                              databoxWide *const slots,
                                              const size_t i) {
    if (slots) {
        flexGetByTypeInto(fe, box, &slots[i]);
    } else {
        flexGetByType(fe, box);
    }
}

/* The self-contained copy DeleteRandomValue hands back for the entry at
 * 'fe', which is about to be deleted, matching multilistPop: BYTES of 8
 * bytes or less come back embedded (DATABOX_BYTES_EMBED, allocated = 0),
 * other values below BYTES decode once with their flags clear, and long
 * BYTES, containers and 96/128-bit values go through flexGetByTypeCopy,
 * which writes a wide value in band when box->big is set and to a 16-byte
 * heap payload the caller owns otherwise. */
static inline void multimapRemovedCopy_(const flexEntry *const fe,
                                        databox *const box) {
    databox found = {{0}};
    flexGetByTypeInto(fe, &found, NULL);
    if (found.type == DATABOX_BYTES) {
        if (found.len <= sizeof(found.data.bytes.embed8)) {
            databox embed = {{0}};
            memcpy(embed.data.bytes.embed8, found.data.bytes.start, found.len);
            embed.type = DATABOX_BYTES_EMBED;
            embed.len = found.len;
            *box = embed;
            return;
        }
    } else if (found.type < DATABOX_BYTES && !databoxIsWideDeferred(&found)) {
        /* Nothing below BYTES borrows from the flex. */
        *box = found;
        return;
    }

    flexGetByTypeCopy(fe, box);
}

/* Decode the entry at 'fe' into a library-owned temporary: a plain box
 * plus a 'slot' that receives a 96/128-bit value (flexGetByTypeInto).
 * With a non-NULL 'referenceContainer', a reference entry is resolved to
 * the value it names, which may also be wide and lands in 'slot' too. */
DK_INLINE_ALWAYS void
multimapGetByTypeInto_(const flexEntry *const fe, databox *const box,
                       databoxWide *const slot,
                       const multimapAtom *const referenceContainer) {
    if (referenceContainer) {
        flexGetByTypeIntoWithReference(fe, box, slot, referenceContainer);
    } else {
        flexGetByTypeInto(fe, box, slot);
    }
}
