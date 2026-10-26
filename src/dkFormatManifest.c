/* dkFormatManifest - every format this build writes and reads
 *
 * Kept apart from dkFormat.c so the stamp primitives depend on nothing;
 * this file knows every format owner.  A format is added here when its
 * owner is added. */

#include "dkFormat.h"

#include "aeadNonceLog.h"
#include "clusterMailbox.h"
#include "clusterRing.h"
#include "flexEnvelope.h"
#include "hmacKeyring.h"
#include "persist.h"

size_t dkFormatManifest(dkFormatSpec *out, size_t cap) {
    static const dkFormatSpec *const fixed[] = {
        &persistFormatSnapshotFile,
        &persistFormatWALFile,
        &clusterMailboxProtocol,
        &flexEnvelopeFormat,
    };
    static const persistOps *const structures[] = {
        &persistOpsFlex,           &persistOpsIntset,
        &persistOpsIntsetBig,      &persistOpsMultiroar,
        &persistOpsHyperloglog,    &persistOpsMultilist,
        &persistOpsMultimap,       &persistOpsMultidict,
        &persistOpsMultiarray,     &persistOpsMultiOrderedSet,
        &persistOpsMultiLRU,       &persistOpsHmacKeyring,
        &persistOpsClusterMailbox, &persistOpsClusterRing,
        &persistOpsAeadNonceLog,
    };

    size_t n = 0;
    for (size_t i = 0; i < countof(fixed); i++, n++) {
        if (out && n < cap) {
            out[n] = *fixed[i];
        }
    }
    for (size_t i = 0; i < countof(structures); i++, n++) {
        if (out && n < cap) {
            out[n] = persistOpsFormat(structures[i]);
        }
    }
    return n;
}
