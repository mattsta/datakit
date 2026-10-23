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
 *     DELETE   [id]         DeleteById
 *
 * multimapAtomDense (PERSIST_TYPE_MULTIMAP_ATOM_DENSE):
 *   snapshot: its base table's (above); the ids in use determine the next
 *             id (one past the highest) and the free set (every id below
 *             it not in use).  Restore refuses a permanent base.
 *   WAL: the same ops; INSERT takes exactly its id out of the free set
 *        (ids it skips over become free).
 *
 * stringPool (PERSIST_TYPE_STRING_POOL):
 *   snapshot: flex [id behaviour, next id, live count, then per string in
 *             id order: id, bytes, count, then every free id below the
 *             next id: REUSE's stack bottom to top (it hands out the most
 *             recently freed first), LOWEST's ascending, none for
 *             PERMANENT].  Every id below the next id is a row or a free
 *             id, so a snapshot's length bounds what restore allocates.
 *   WAL: INSERT [id, bytes]   a new string under the id the pool hands out
 *                             next (refused otherwise, or when present)
 *        NUM_INCR [id, +1|-1] Retain / Release (the last one frees)
 *        CLEAR                Reset
 *
 * atomPool (PERSIST_TYPE_ATOM_POOL):
 *   snapshot: flex [backend (HASH 0, TREE 1), id behaviour, the backend's
 *             snapshot as bytes: stringPool's, or multimapAtom's
 *             (multimapAtomDense's for lowest-first TREE)].  Restore
 *             refuses a backend whose own id behaviour differs.
 *   WAL: INSERT [id, bytes], NUM_INCR [id, +1|-1] with the pool's 1-based
 *        ids, replayed through the backend's ops; CLEAR (Reset). */

#include "databox.h"
#include "persist.h"

extern const persistOps persistOpsMultimapAtom;
extern const persistOps persistOpsMultimapAtomDense;
extern const persistOps persistOpsStringPool;
extern const persistOps persistOpsAtomPool;

/* Whether a persisted value is one an atom may hold: a number, true,
 * false, null or bytes (shared by the atom descriptors) */
bool atomPersistValueOk(const databox *value);
