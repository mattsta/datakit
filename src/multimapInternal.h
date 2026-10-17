#pragma once

/* Helpers shared by the multimap tiers.  Not part of the public API. */

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
