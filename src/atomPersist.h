#pragma once

/* persistOps for the atom tables.  Every id is kept exactly: a restored
 * table resolves every id it had and hands out the same ids next.
 *
 * multimapAtom (PERSIST_TYPE_MULTIMAP_ATOM):
 *   snapshot: flex [flags (bits 0-1: multimapAtomIds, bits 2-3:
 *             multimapAtomIndex, bit 4: multimapAtomStore), next id, then
 *             per atom in id order: id,
 *             value (stored exactly: "22" stays bytes), count (0-based: 0
 *             is one reference)].  The index and LOWEST's free set (the
 *             ids below the next id with no row) are rebuilt, never
 *             stored.
 *   restore:  refuses ids out of order or at or past the next id, a value
 *             bound twice, a reference or container as a value, a count in
 *             a permanent table, and a LOWEST table whose highest id is not
 *             the next id less one.
 *   WAL (args: const databox *const *, ids UNSIGNED_64):
 *     INSERT   [id, value]  a new atom with one reference under that id
 *                           (refused when the id or the value is bound;
 *                           under LOWEST the id leaves the free set and
 *                           the ids it skips over join it)
 *     NUM_INCR [id, +1|-1]  one reference more / fewer (the last one
 *                           deletes the atom, as Release does)
 *     DELETE   [id]         DeleteById
 *     CLEAR    (no body)    Reset: every atom gone, the settings kept */

#include "databox.h"
#include "persist.h"

extern const persistOps persistOpsMultimapAtom;

/* Whether a persisted value is one an atom may hold: a number, true,
 * false, null or bytes (shared by the atom descriptors) */
bool atomPersistValueOk(const databox *value);
