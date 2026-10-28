#pragma once

#include "databox.h"
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * atomPool - Unified String Interning Interface
 * ====================================================================
 *
 * Abstract interface for string interning that supports multiple backends:
 *   - ATOM_POOL_HASH:  stringPool (hash table + id array) - fast, more memory
 *   - ATOM_POOL_TREE:  multimapAtom (index + id rows) - slower, compact
 *
 * This allows data structures like multiOrderedSet to use interning
 * without being coupled to a specific implementation. You can switch
 * backends transparently based on your performance/memory trade-offs.
 *
 * USAGE
 * -----
 *   // Create with desired backend
 *   atomPool *pool = atomPoolNew(ATOM_POOL_HASH);  // or ATOM_POOL_TREE
 *
 *   // Intern a string (increments refcount, returns ID)
 *   uint64_t id = atomPoolIntern(pool, &memberStr);
 *
 *   // Lookup string by ID
 *   databox resolved;
 *   atomPoolLookup(pool, id, &resolved);
 *
 *   // Reference counting
 *   atomPoolRetain(pool, id);   // Increment
 *   atomPoolRelease(pool, id);  // Decrement (frees if 0)
 *
 *   atomPoolFree(pool);
 *
 * CHOOSING
 * --------
 *   HASH: id -> string is an array read and every operation is a hash
 *   lookup or less; about 1.8x TREE's memory per atom.  TREE: the compact
 *   one (its bytes are close to the string's own), with each operation a
 *   search of its index.  Measured numbers and a guide:
 *   docs/modules/multi/ATOMS.md (perfRegression rows atomPoolHash and
 *   atomPoolTree).
 *
 * LIMITATIONS
 * -----------
 *   - atomPoolReset() empties either backend (TREE recreates its store)
 *   - IDs are 1-based (0 means error/invalid) for both backends
 *   - Refcounts via API are 1-based (refcount = actual reservations)
 *   - TREE backend has higher per-operation overhead but lower memory
 *
 * IMPLEMENTATION NOTE
 * -------------------
 *   TREE backend (multimapAtom) uses 0-based internal refcounts for memory
 *   efficiency: DATABOX_FALSE (0) = 1 byte vs 3 bytes for encoding "1".
 *   The atomPool API layer translates to 1-based for consistent semantics.
 */

/* Backend implementation types */
typedef enum atomPoolType {
    ATOM_POOL_HASH = 0, /* stringPool: fastest, more memory */
    ATOM_POOL_TREE = 1, /* multimapAtom: compact, slower */
} atomPoolType;

/* The id behaviour, fixed at creation; both backends offer all three and
 * none adds a byte per atom:
 *   ATOM_POOL_IDS_REUSE      the last release frees an atom; its id is
 *                            reused (HASH: most recently freed first;
 *                            TREE: ids freed from the top, and all ids
 *                            when the pool empties).  atomPoolNew.
 *   ATOM_POOL_IDS_PERMANENT  like Erlang atoms: an atom lives until Reset
 *                            or Free; Retain and Release change nothing
 *                            (Refcount reports 1); no id is reused.
 *   ATOM_POOL_IDS_LOWEST     the last release frees an atom; a new atom
 *                            always gets the lowest id not in use (a
 *                            multiroar of free ids: ~2 bytes per hole). */
typedef enum atomPoolIds {
    ATOM_POOL_IDS_REUSE = 0,
    ATOM_POOL_IDS_PERMANENT,
    ATOM_POOL_IDS_LOWEST,
} atomPoolIds;

/* Forward declaration */
typedef struct atomPool atomPool;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

/* Create a new atom pool with specified backend */
[[nodiscard]] atomPool *atomPoolNew(atomPoolType type);

/* Create with a backend and an id behaviour; NULL for an unknown one */
[[nodiscard]] atomPool *atomPoolNewWithIds(atomPoolType type, atomPoolIds ids);
atomPoolIds atomPoolGetIds(const atomPool *pool);

/* Create with default backend (ATOM_POOL_HASH for speed) */
[[nodiscard]] atomPool *atomPoolNewDefault(void);

/* Free the pool and all interned strings */
void atomPoolFree(atomPool *pool);

/* Empty the pool: every atom gone, ids start over.  HASH clears its
 * tables in place (keeping their memory); TREE replaces its table with a
 * new empty one. */
void atomPoolReset(atomPool *pool);

/* ====================================================================
 * Interning Operations
 * ==================================================================== */

/* Intern a string, returning its ID.
 * If string already exists, increments refcount and returns existing ID.
 * If new, creates entry with refcount=1.
 * Returns 0 on error. */
uint64_t atomPoolIntern(atomPool *pool, const databox *str);

/* Get ID for string without incrementing refcount.
 * Returns 0 if string not found. */
uint64_t atomPoolGetId(const atomPool *pool, const databox *str);

/* Check if string exists in pool */
bool atomPoolExists(const atomPool *pool, const databox *str);

/* ====================================================================
 * Lookup Operations
 * ==================================================================== */

/* Lookup string by ID.
 * Returns true if found, fills 'str' with the interned string.
 * The returned databox points to internal storage - do not modify. */
bool atomPoolLookup(const atomPool *pool, uint64_t id, databox *str);

/* ====================================================================
 * Reference Counting
 * ==================================================================== */

/* Increment refcount for ID */
void atomPoolRetain(atomPool *pool, uint64_t id);

/* Decrement refcount. Returns true if entry was freed (refcount hit 0). */
bool atomPoolRelease(atomPool *pool, uint64_t id);

/* Get current refcount for ID (0 if not found) */
uint64_t atomPoolRefcount(const atomPool *pool, uint64_t id);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/* Number of unique strings currently interned */
size_t atomPoolCount(const atomPool *pool);

/* Total memory used by pool */
size_t atomPoolBytes(const atomPool *pool);

/* Get the backend type */
atomPoolType atomPoolGetType(const atomPool *pool);

/* Get human-readable backend name */
const char *atomPoolTypeName(atomPoolType type);

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
void atomPoolRepr(const atomPool *pool);
int atomPoolTest(int argc, char *argv[]);
#endif
