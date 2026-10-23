#pragma once

/* persistOps for the atom tables.  Every id is kept exactly: a restored
 * table resolves every id it had and hands out the same ids next.
 *
 * multimapAtom (PERSIST_TYPE_MULTIMAP_ATOM):
 *   snapshot: flex [flags (bit 0: permanent), next id, then per atom in id
 *             order: id, value (stored exactly: "22" stays bytes),
 *             count (0-based: 0 is one reference)]
 *   restore:  refuses ids out of order or at or past the next id, a value
 *             bound twice, a reference or container as a value, and a
 *             count in a permanent table.
 *   WAL (args: const databox *const *, ids UNSIGNED_64):
 *     INSERT   [id, value]  a new atom with one reference under that id
 *                           (refused when the id or the value is bound)
 *     NUM_INCR [id, +1|-1]  one reference more / fewer (the last one
 *                           deletes the atom, as Release does)
 *     DELETE   [id]         DeleteById */

#include "persist.h"

extern const persistOps persistOpsMultimapAtom;
