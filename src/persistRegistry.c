/* persistRegistry - every structure type this build persists
 *
 * The one list of persist descriptors: the format manifest names their
 * formats from it, and readers that start from a file (operator tools)
 * find a file's descriptor in it by the type its header names.  A
 * descriptor is added here when its owner is added; the persistence
 * contract fails when a descriptor is missing here or has no contract
 * type. */

#include "persist.h"

#include "aeadNonceLog.h"
#include "atomPersist.h"
#include "clusterMailbox.h"
#include "clusterRing.h"
#include "fenwick/fenwickPersist.h"
#include "hmacKeyring.h"
#include "multiFenwick.h"
#include "multiFenwick2d.h"
#include "multiFenwickFlat.h"
#include "segment/segmentPersist.h"

static const persistOps *const persistRegistry_[] = {
    &persistOpsFlex,
    &persistOpsIntset,
    &persistOpsIntsetBig,
    &persistOpsMultiroar,
    &persistOpsHyperloglog,
    &persistOpsMultilist,
    &persistOpsMultimap,
    &persistOpsMultidict,
    &persistOpsMultiarray,
    &persistOpsMultiOrderedSet,
    &persistOpsMultiLRU,
    &persistOpsHmacKeyring,
    &persistOpsClusterMailbox,
    &persistOpsClusterRing,
    &persistOpsAeadNonceLog,
    &persistOpsIdRows,
    &persistOpsColdmap,
    &persistOpsMultimapAtom,
    &persistOpsMultiFenwick,
    &persistOpsMultiFenwickFlat,
    &persistOpsMultiFenwick2d,
    &persistOpsFenwickI16,
    &persistOpsFenwickI32,
    &persistOpsFenwickI64,
    &persistOpsFenwickI128,
    &persistOpsFenwickU16,
    &persistOpsFenwickU32,
    &persistOpsFenwickU64,
    &persistOpsFenwickU128,
    &persistOpsFenwickFloat,
    &persistOpsFenwickDouble,
    &persistOpsFenwick2dI16,
    &persistOpsFenwick2dI32,
    &persistOpsFenwick2dI64,
    &persistOpsFenwick2dI128,
    &persistOpsFenwick2dU16,
    &persistOpsFenwick2dU32,
    &persistOpsFenwick2dU64,
    &persistOpsFenwick2dU128,
    &persistOpsFenwick2dFloat,
    &persistOpsFenwick2dDouble,
    &persistOpsSegmentI16,
    &persistOpsSegmentI32,
    &persistOpsSegmentI64,
    &persistOpsSegmentI128,
    &persistOpsSegmentU16,
    &persistOpsSegmentU32,
    &persistOpsSegmentU64,
    &persistOpsSegmentU128,
    &persistOpsSegmentFloat,
    &persistOpsSegmentDouble,
};

size_t persistRegistry(const persistOps *const **ops) {
    *ops = persistRegistry_;
    return countof(persistRegistry_);
}

const persistOps *persistOpsForType(const persistType type) {
    for (size_t i = 0; i < countof(persistRegistry_); i++) {
        if (persistRegistry_[i]->type == type) {
            return persistRegistry_[i];
        }
    }

    return nullptr;
}
