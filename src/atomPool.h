#pragma once

#include "databox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * atomPool - Unified String Interning Interface
 * ====================================================================
 *
 * Abstract interface for string interning that supports multiple backends:
 *   - ATOM_POOL_HASH:  O(1) hash-based (stringPool) - fast, more memory
 *   - ATOM_POOL_TREE:  O(log n) tree-based (multimapAtom) - slower, compact
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
 * PERFORMANCE COMPARISON
 * ----------------------
 *   Operation          | HASH (stringPool) | TREE (multimapAtom)
 *   -------------------|-------------------|--------------------
 *   Intern             | O(1) avg          | O(log n)
 *   Lookup by string   | O(1) avg          | O(log n)
 *   Lookup by ID       | O(1) array        | O(log n)
 *   Retain/Release     | O(1)              | O(log n)
 *   Iteration          | ~2 cycles/op      | ~10 cycles/op (5-6x slower!)
 *   Memory/entry       | ~84 bytes         | ~22 bytes (3-4x smaller)
 *
 * BACKEND SELECTION GUIDE
 * -----------------------
 *   Use ATOM_POOL_HASH when:
 *     - Lookup speed is critical (100+ M ops/s for ID lookup)
 *     - You iterate over pooled members frequently
 *     - You have many lookups per intern
 *     - Memory is not a primary constraint
 *
 *   Use ATOM_POOL_TREE when:
 *     - Memory efficiency is critical (3-4x less memory)
 *     - Write-heavy workload (intern/release frequent)
 *     - Small to medium pools where O(log n) is acceptable
 *     - Iteration is rare or not performance-critical
 *
 *   CRITICAL: TREE is 5-10x slower for iteration than HASH!
 *   If your workload iterates frequently, use HASH despite memory cost.
 *
 * LIMITATIONS
 * -----------
 *   - atomPoolReset() only works with HASH backend; TREE backend no-op
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
    ATOM_POOL_HASH = 0, /* stringPool - O(1) hash, more memory */
    ATOM_POOL_TREE = 1, /* multimapAtom - O(log n) tree, compact */
} atomPoolType;

/* Forward declaration */
typedef struct atomPool atomPool;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

/* Create a new atom pool with specified backend */
atomPool *atomPoolNew(atomPoolType type);

/* Create with default backend (ATOM_POOL_HASH for speed) */
atomPool *atomPoolNewDefault(void);

/* Free the pool and all interned strings */
void atomPoolFree(atomPool *pool);

/* Reset pool to empty state (keeps allocated memory).
 * Note: TREE backend does not support reset - this is a no-op for TREE. */
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
