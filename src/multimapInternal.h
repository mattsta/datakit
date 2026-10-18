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

/* Decode the entry at 'fe' into a library-owned temporary that can hold
 * any value, including a 96/128-bit one (see flexGetByTypeBig).  With a
 * non-NULL 'referenceContainer', a reference entry is resolved to the
 * value it names, which may also be wide. */
static inline void
multimapGetByTypeBig_(const flexEntry *const fe, databoxBig *const box,
                      const multimapAtom *const referenceContainer) {
    flexGetByTypeBig(fe, box);
    if (referenceContainer &&
        box->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL) {
        multimapAtomLookupConvertBig(referenceContainer, box);
    }
}
