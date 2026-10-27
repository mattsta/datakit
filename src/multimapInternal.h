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

/* Report the value entries of the row whose key entry is 'fe', which is
 * about to be deleted, into 'removedOut' ('elementsPerEntry - 1' slots;
 * NULL is a no-op).  Without 'take', under replacedOut's rules: numbers by
 * value; a BYTES or container slot keeps its type and length but points
 * into the map, so it must not be read after the delete; a 96/128-bit
 * value is type-only.  With 'take', as DeleteRandomValue hands values
 * back (flexGetByTypeDetached): self-contained, owned by the caller. */
static inline void
multimapRemovedOutFill_(const flex *map, const flexEntry *fe,
                        const multimapElements elementsPerEntry,
                        databox *const removedOut, const bool take) {
    if (!removedOut) {
        return;
    }

    for (multimapElements i = 1; i < elementsPerEntry; i++) {
        fe = flexNext(map, (flexEntry *)fe);
        if (take) {
            flexGetByTypeDetached(fe, &removedOut[i - 1]);
        } else {
            removedOut[i - 1] = (databox){};
            flexGetByTypeInto(fe, &removedOut[i - 1], nullptr);
        }
    }
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
