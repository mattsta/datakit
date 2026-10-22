#pragma once

#include "multimapAtom.h"

/* multimapAtomDense: an atom table that always hands out the lowest id not
 * in use.
 *
 * The same reference-counted interning as multimapAtom (see multimapAtom.h
 * for references and lookups), with a different id behaviour: every freed
 * id is reused, lowest first, so the ids in use are as dense as they can
 * be and never exceed the high-water mark of the live count.  Use it when
 * ids index something sized by their range (arrays, bitmaps, compact
 * reference encodings) and atoms come and go in any order; multimapAtom's
 * reusing behaviour only reclaims ids freed from the top.
 *
 * MEMORY AND TIME
 * - The atoms cost what multimapAtom's do; nothing is added per atom.
 * - Freed ids below the highest live id ("holes") are kept in a multiroar
 *   free set, created on the first hole and released when the last hole
 *   fills: about 2 bytes per isolated hole, less for runs of holes, and
 *   none while there are no holes.
 * - A new atom takes the set's minimum (O(1) per chunk) or the next unused
 *   id; freeing the highest id lowers the next unused id past any holes
 *   directly below it.
 *
 * IDS
 * - multimapAtomDenseNextId is the id the next new atom gets: the lowest
 *   id not in use.
 * - An id is valid while its atom exists.  A caller must not keep a bare
 *   id after releasing its references: the next new atom may get it.
 * - The read calls (Lookup*, Count) take the base table:
 *   multimapAtomLookup(multimapAtomDenseBase(d), ...).  Write only through
 *   the calls below, so the free set stays exact.
 *
 * Not thread-safe. */

typedef struct multimapAtomDense multimapAtomDense;

multimapAtomDense *multimapAtomDenseNew(void);
void multimapAtomDenseFree(multimapAtomDense *d);

/* The underlying table, for read-only multimapAtom calls */
const multimapAtom *multimapAtomDenseBase(const multimapAtomDense *d);

/* Intern 'box' as a new atom (one reference) with the lowest free id and
 * replace 'box' with its reference, as multimapAtomInsertConvert.  The
 * value must not be in the table yet; use InsertIfNewConvert otherwise. */
void multimapAtomDenseInsertConvert(multimapAtomDense *d, databox *box);

/* As multimapAtomInsertIfNewConvert(AndRetain): an existing value's
 * reference (retained once more by AndRetain) or a new atom's.  Returns
 * true when the value already existed; key->created is set to match. */
bool multimapAtomDenseInsertIfNewConvert(multimapAtomDense *d, databox *key);
bool multimapAtomDenseInsertIfNewConvertAndRetain(multimapAtomDense *d,
                                                  databox *key);

/* One more reference: to the value 'key' (interned if new), or to 'id' */
void multimapAtomDenseRetain(multimapAtomDense *d, const databox *key);
void multimapAtomDenseRetainById(multimapAtomDense *d, uint64_t id);

/* One reference fewer; the last one deletes the atom and frees its id.
 * Return true when the atom was deleted (false also for a value or id not
 * in the table). */
bool multimapAtomDenseRelease(multimapAtomDense *d, const databox *key);
bool multimapAtomDenseReleaseById(multimapAtomDense *d, uint64_t id);

/* Delete an atom whatever its count and free its id.  True if it existed. */
bool multimapAtomDenseDelete(multimapAtomDense *d, const databox *key);
bool multimapAtomDenseDeleteById(multimapAtomDense *d, uint64_t id);

/* The id the next new atom gets: the lowest id not in use */
uint64_t multimapAtomDenseNextId(const multimapAtomDense *d);

/* Atoms in the table; bytes of the table plus its free set */
size_t multimapAtomDenseCount(const multimapAtomDense *d);
size_t multimapAtomDenseBytes(const multimapAtomDense *d);

#ifdef DATAKIT_TEST
int multimapAtomDenseTest(int argc, char *argv[]);
#endif
