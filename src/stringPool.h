#pragma once

#include "databox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * stringPool - Fast O(1) String Interning with Reference Counting
 * ====================================================================
 *
 * A string interning system optimized for high-performance lookups.
 * Uses hash table for O(1) string→ID mapping and array for O(1) ID→string.
 *
 * USE CASE
 * --------
 * When the same strings are stored in multiple data structures, interning
 * reduces memory by storing each unique string once and using small integer
 * IDs as references elsewhere.
 *
 * PERFORMANCE
 * -----------
 * - Intern (insert):     O(1) average
 * - Lookup by string:    O(1) average (hash table)
 * - Lookup by ID:        O(1) (direct array access)
 * - Release:             O(1)
 *
 * MEMORY
 * ------
 * Per interned string: string bytes + ~24 bytes overhead
 * IDs are uint64_t but can be varint-encoded when stored in flex (~1-9 bytes)
 *
 * REFERENCE COUNTING
 * ------------------
 * Each intern() increments refcount. Each release() decrements.
 * String is freed when refcount reaches 0.
 *
 * THREAD SAFETY
 * -------------
 * NOT thread-safe. External synchronization required.
 *
 * EXAMPLE
 * -------
 *   stringPool *pool = stringPoolNew();
 *
 *   // Intern a string (refcount=1)
 *   databox member = databoxNewBytesString("player123");
 *   uint64_t id = stringPoolIntern(pool, &member);
 *
 *   // Use ID in other structures
 *   myScoreMap[id] = 100;
 *
 *   // Lookup string by ID
 *   databox resolved;
 *   stringPoolLookup(pool, id, &resolved);  // resolved = "player123"
 *
 *   // Retain (increment refcount) when storing in another place
 *   stringPoolRetain(pool, id);
 *
 *   // Release when done (decrements refcount, frees if 0)
 *   stringPoolRelease(pool, id);
 *   stringPoolRelease(pool, id);  // refcount=0, string freed
 *
 *   stringPoolFree(pool);
 */

typedef struct stringPool stringPool;

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

stringPool *stringPoolNew(void);
void stringPoolFree(stringPool *pool);
void stringPoolReset(stringPool *pool);

/* ====================================================================
 * Interning Operations
 * ==================================================================== */

/* Intern a string, returning its ID.
 * If string already exists, increments refcount and returns existing ID.
 * If new, creates entry with refcount=1.
 * Returns 0 on error (allocation failure). */
uint64_t stringPoolIntern(stringPool *pool, const databox *str);

/* Get ID for string without incrementing refcount.
 * Returns 0 if string not found. */
uint64_t stringPoolGetId(const stringPool *pool, const databox *str);

/* Check if string exists in pool */
bool stringPoolExists(const stringPool *pool, const databox *str);

/* ====================================================================
 * Lookup Operations
 * ==================================================================== */

/* Lookup string by ID.
 * Returns true if found, fills 'str' with the interned string.
 * The returned databox points to internal storage - do not modify. */
bool stringPoolLookup(const stringPool *pool, uint64_t id, databox *str);

/* ====================================================================
 * Reference Counting
 * ==================================================================== */

/* Increment refcount for ID */
void stringPoolRetain(stringPool *pool, uint64_t id);

/* Decrement refcount. Returns true if entry was freed (refcount hit 0). */
bool stringPoolRelease(stringPool *pool, uint64_t id);

/* Get current refcount for ID (0 if not found) */
uint64_t stringPoolRefcount(const stringPool *pool, uint64_t id);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/* Number of unique strings currently interned */
size_t stringPoolCount(const stringPool *pool);

/* Total memory used by pool */
size_t stringPoolBytes(const stringPool *pool);

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
void stringPoolRepr(const stringPool *pool);
int stringPoolTest(int argc, char *argv[]);
#endif
