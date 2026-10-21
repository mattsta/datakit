#pragma once

#include "databox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Multi-Type Fenwick Tree (Databox-Based Binary Indexed Tree)
 * ====================================================================
 *
 * OVERVIEW
 * --------
 * A memory-efficient data structure for dynamic cumulative frequency tables
 * that supports ANY numeric type (int64_t, uint64_t, float, double, etc.).
 * Unlike the standard fenwick which uses int64_t arrays, multiFenwick uses
 * multilist for storage and databox for all values, eliminating datatype
 * decay and enabling type flexibility.
 *
 * KEY FEATURES
 * ------------
 * - Supports any numeric databox type (SIGNED_64, UNSIGNED_64, FLOAT_32,
 * DOUBLE_64)
 * - Integer sums are exact; floating sums run in double precision
 * - Uses multilist for efficient storage
 * - O(log n) cell accesses per operation, but each access walks a flex
 *   node, so operations run hundreds of times slower than the standard
 *   fenwick; multiFenwickFlat (multiFenwickFlat.h) is the same tree with
 *   O(1) cells, at about 9 bytes per cell instead of about 4
 * - Tiered architecture for optimal memory usage
 *
 * OPERATIONS
 * ----------
 * - Point update:  O(log n) - add delta to single element
 * - Prefix query:  O(log n) - sum elements [0, idx]
 * - Range query:   O(log n) - sum elements [left, right]
 * - Point query:   O(log n) - get value at single index
 *
 * ARCHITECTURE
 * ------------
 * Uses multilist for storage, which automatically handles tiering and
 * memory management internally. This simplifies the implementation to
 * a single structure with no manual tier transitions required.
 *
 * USAGE EXAMPLE - Dynamic Range Sum Queries with Doubles
 * --------------------------------------------------------
 *   multiFenwick *mfw = multiFenwickNew();
 *
 *   // Build from array of doubles
 *   databox values[5];
 *   for (size_t i = 0; i < 5; i++) {
 *       DATABOX_SET_DOUBLE(&values[i], (double)(i + 1) * 1.5);
 *   }
 *
 *   for (size_t i = 0; i < 5; i++) {
 *       multiFenwickUpdate(&mfw, i, &values[i]);
 *   }
 *
 *   // Query sum [0, 3]
 *   databox sum;
 *   multiFenwickQuery(mfw, 3, &sum);
 *   printf("Sum: %f\n", sum.data.d64);
 *
 *   // Update: add delta
 *   databox delta = DATABOX_DOUBLE(10.5);
 *   multiFenwickUpdate(&mfw, 2, &delta);
 *
 *   multiFenwickFree(mfw);
 *
 * USAGE EXAMPLE - Integer Frequency Table
 * ----------------------------------------
 *   multiFenwick *freq = multiFenwickNew();
 *
 *   // Increment frequency at position 42
 *   databox one = DATABOX_SIGNED(1);
 *   multiFenwickUpdate(&freq, 42, &one);
 *
 *   // Get cumulative frequency
 *   databox cumulative;
 *   multiFenwickQuery(freq, 42, &cumulative);
 *
 *   // Get exact frequency
 *   databox exact;
 *   multiFenwickGet(freq, 42, &exact);
 *
 *   multiFenwickFree(freq);
 *
 * INDEXING
 * --------
 * Uses 0-based indexing externally (C convention).
 * Internally converted to 1-based (BIT requirement).
 *
 * THREAD SAFETY
 * -------------
 * NOT thread-safe. External synchronization required.
 *
 * VALUES
 * ------
 * Accepted: SIGNED_64, UNSIGNED_64, and finite FLOAT_32 or DOUBLE_64.
 * Every other input is refused, before anything changes: VOID, BYTES,
 * booleans, pointers, containers, 96/128-bit values (SIGNED_128,
 * UNSIGNED_128), NaN and +/-infinity.  Update and Set return false,
 * NewFromArray returns NULL and LowerBound returns SIZE_MAX, so nothing
 * else is ever stored or returned.
 *
 * Results are typed by value, not by the type that went in (storage
 * keeps values canonically):
 * - an integer result is SIGNED_64 when negative, UNSIGNED_64 otherwise;
 * - a result with any floating part is DOUBLE_64.
 *
 * Integer arithmetic is exact.  Each stored cell holds the sum of a
 * block of consecutive elements; the cell at the capacity holds the
 * total of all of them.  Every cell must fit [INT64_MIN, UINT64_MAX]:
 * an Update or Set that would push one outside is refused, and
 * NewFromArray refuses such an array.  With non-negative elements every
 * cell is at most the total, so the rule is just "the total fits in a
 * uint64_t".  Query, RangeQuery and Get compute their result exactly
 * from the cells and return false (result VOID) only when that result
 * itself is outside [INT64_MIN, UINT64_MAX]: a range or an element can
 * be returned even when a prefix sum around it can not.
 *
 * Floating arithmetic runs in double, FLOAT_32 inputs included, and is
 * rounded as double arithmetic is.  A cell or result that would not be
 * finite is treated as out of range, as above.  In a tree that mixes
 * integer and floating values, a cell that has taken any floating value
 * is a double; integer cells stay exact, and a sum keeps its integer
 * cells' part exact until the one final conversion to double.  Keep a
 * tree all-integer when results must be exact above 2^53.
 *
 * NULL AND RANGE HANDLING
 * -----------------------
 * - A NULL tree is an empty tree: Count and Bytes return 0, Query, Get
 *   and RangeQuery return false, LowerBound returns SIZE_MAX, Clear and
 *   Free do nothing.  Update and Set create the tree.
 * - A NULL value, delta, target or 'mfw' pointer-to-pointer is refused.
 * - A NULL 'result' makes Query, Get and RangeQuery return false.  On
 *   every other false return a non-NULL 'result' is set to VOID.
 * - Query and Get need idx < count; RangeQuery needs
 *   left <= right < count.  Update and Set take any idx up to
 *   MULTIFENWICK_INDEX_MAX and extend count to idx + 1.
 *
 * LIMITATIONS
 * -----------
 * - Does not support range updates directly
 * - Does not support minimum/maximum queries (use segment tree)
 * - Indexes are 0 to MULTIFENWICK_INDEX_MAX
 */

/* Opaque multiFenwick tree type */
typedef struct multiFenwick multiFenwick;

/* ====================================================================
 * CREATION & DESTRUCTION
 * ==================================================================== */

/* Create new empty multiFenwick tree */
multiFenwick *multiFenwickNew(void);

/* Create multiFenwick tree from databox array: element i is values[i]
 * Time: O(n)
 * count 0 gives a new empty tree ('values' is not read and may be NULL).
 * Returns NULL, with nothing allocated or left behind, when 'values' is
 * NULL with a non-zero count, when count - 1 is above
 * MULTIFENWICK_INDEX_MAX, when any element is not an accepted value
 * (VOID included: write a zero for a zero), or when a stored block sum
 * would be out of range (see VALUES above), or on allocation failure. */
multiFenwick *multiFenwickNewFromArray(const databox *values, size_t count);

/* Free multiFenwick tree */
void multiFenwickFree(multiFenwick *mfw);

/* ====================================================================
 * CORE OPERATIONS
 * ==================================================================== */

/* Largest index Update and Set accept.  Cells are addressed with the
 * multilist's 32-bit node index, and capacity is the smallest power of
 * two at or above the count, so it stays at or below 2^30. */
#define MULTIFENWICK_INDEX_MAX ((((uint64_t)1) << 30) - 1)

/* Update: add delta to element at idx
 * Time: O(log n)
 * mfw: pointer-to-pointer (created if NULL)
 * idx: 0-based index, at most MULTIFENWICK_INDEX_MAX; past the end it
 *      extends the count to idx + 1 (the elements between are zero)
 * delta: value to add (negative to subtract)
 * Returns true on success.  Returns false for a NULL 'mfw' or 'delta',
 * a delta that is not an accepted value (see VALUES above), an index
 * above MULTIFENWICK_INDEX_MAX, a stored block sum that would go out of
 * range, or a failed allocation of a new tree.
 * Transactional: every check, and the read and sum of every cell the
 * update touches, happens before the first write, and the writes cannot
 * fail.  So a false return leaves every cell, the count and the capacity
 * unchanged, and a NULL '*mfw' stays NULL. */
bool multiFenwickUpdate(multiFenwick **mfw, size_t idx, const databox *delta);

/* Query: compute prefix sum [0, idx]
 * Time: O(log n)
 * Returns true on success.  Returns false for a NULL tree or result,
 * idx >= count (so always on an empty tree), or a sum out of range;
 * a non-NULL result is then VOID. */
bool multiFenwickQuery(const multiFenwick *mfw, size_t idx, databox *result);

/* Range query: compute sum [left, right] (inclusive), exactly, from the
 * two prefix sums around it
 * Time: O(log n)
 * Returns true on success.  Returns false for a NULL tree or result,
 * left > right, right >= count, or a sum out of range; a non-NULL result
 * is then VOID. */
bool multiFenwickRangeQuery(const multiFenwick *mfw, size_t left, size_t right,
                            databox *result);

/* Get single element value at idx
 * Time: O(log n), from the cells covering the element alone (no prefix
 * sum is formed, so an element is returned whenever it is in range)
 * Returns true on success.  Returns false for a NULL tree or result,
 * idx >= count, or a value out of range; a non-NULL result is then
 * VOID. */
bool multiFenwickGet(const multiFenwick *mfw, size_t idx, databox *result);

/* Set single element to exact value (not delta)
 * Time: O(log n)
 * One transactional Update by value - current, with that difference kept
 * exact (it may need more than 64 bits, as from INT64_MIN to
 * UINT64_MAX).  An element past the end, or of a NULL tree, is zero.
 * Returns false, changing nothing, for the same reasons as Update. */
bool multiFenwickSet(multiFenwick **mfw, size_t idx, const databox *value);

/* ====================================================================
 * METADATA & INSPECTION
 * ==================================================================== */

/* Get number of elements */
size_t multiFenwickCount(const multiFenwick *mfw);

/* Get total bytes used (including metadata and multilist storage) */
size_t multiFenwickBytes(const multiFenwick *mfw);

/* ====================================================================
 * ADVANCED OPERATIONS
 * ==================================================================== */

/* Find smallest index with cumulative sum >= target
 * Time: O(log n) (binary lifting down the tree's cells)
 * Comparisons are exact, integers against doubles included.
 * Returns SIZE_MAX for a NULL or empty tree, a NULL target, a target
 * that is not an accepted value, or when no prefix sum reaches the
 * target.
 * With non-negative elements the prefix sums never fall and the result
 * is the smallest such index; a target at or below the first element
 * (any zero or negative target included) returns 0.  With negative
 * elements the prefix sums
 * can fall; a returned index i then still has prefix(i) >= target and
 * either i == 0 or prefix(i - 1) < target, but it need not be the
 * smallest, and SIZE_MAX can be returned although some prefix sum
 * reaches the target. */
size_t multiFenwickLowerBound(const multiFenwick *mfw, const databox *target);

/* Reset all values to zero; the count and capacity stay as they were
 * Time: O(n) */
void multiFenwickClear(multiFenwick *mfw);

/* ====================================================================
 * TESTING & DEBUG
 * ==================================================================== */

#ifdef DATAKIT_TEST
/* Print debug representation */
void multiFenwickRepr(const multiFenwick *mfw);

/* Run comprehensive tests */
int multiFenwickTest(int argc, char *argv[]);
#endif

/*
 * multiFenwick Tree (Databox-Based Binary Indexed Tree)
 * Based on Fenwick Tree algorithm by Peter M. Fenwick (1994)
 */
