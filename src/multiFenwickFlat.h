#pragma once

#include "databox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Flat Multi-Type Fenwick Tree (Databox-Based Binary Indexed Tree)
 * ====================================================================
 *
 * OVERVIEW
 * --------
 * The same tree as multiFenwick (multiFenwick.h): the same operations,
 * accepted values, exact arithmetic, typed results, NULL handling and
 * transactional writes, with the same index bound.  Only the cell storage
 * differs:
 *
 * - multiFenwick keeps its cells in a multilist of flex nodes: compact
 *   (about 2-9 bytes per cell), but every cell access walks a flex node,
 *   so each operation costs O(log n) cell walks.
 * - multiFenwickFlat keeps its cells in flat arrays: 8 bytes of value
 *   bits per cell, plus a 1-byte kind (signed, unsigned or double) per
 *   cell once any cell holds a value above INT64_MAX or a double.  Every
 *   cell access is O(1), so every operation is O(log n) like an int64
 *   Fenwick tree; while every cell is an int64, sums also skip the
 *   float bookkeeping.
 *
 * Choose multiFenwickFlat when operation speed matters more than 8 (or
 * 9) bytes per cell; choose multiFenwick for the most compact tree.
 *
 * OPERATIONS
 * ----------
 * - Point update:  O(log n) - add delta to single element
 * - Prefix query:  O(log n) - sum elements [0, idx]
 * - Range query:   O(log n) - sum elements [left, right]
 * - Point query:   O(log n) - get value at single index
 *
 * STORAGE
 * -------
 * The arrays hold 'capacity' cells, the smallest power of two at or
 * above the count, and grow by doubling, so appending indexes one by one
 * costs amortized O(1) growth.  Cells past the count hold zero.  The
 * per-cell kinds array is created by the first cell that is not an int64
 * and dropped again by Clear.
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
 * VALUES, NULL AND RANGE HANDLING
 * -------------------------------
 * Exactly as multiFenwick.h describes under VALUES and NULL AND RANGE
 * HANDLING, with multiFenwickFlat in place of multiFenwick and
 * MULTIFENWICK_FLAT_INDEX_MAX in place of MULTIFENWICK_INDEX_MAX.  The
 * two trees give identical results, result types included, for the same
 * sequence of calls; the tests check that differentially.
 */

/* Opaque flat multiFenwick tree type */
typedef struct multiFenwickFlat multiFenwickFlat;

/* ====================================================================
 * CREATION & DESTRUCTION
 * ==================================================================== */

/* Create new empty tree (no cells are allocated until the first write) */
[[nodiscard]] multiFenwickFlat *multiFenwickFlatNew(void);

/* Create tree from databox array: element i is values[i]
 * Time: O(n)
 * count 0 gives a new empty tree ('values' is not read and may be NULL).
 * Returns NULL, with nothing allocated or left behind, when 'values' is
 * NULL with a non-zero count, when count - 1 is above
 * MULTIFENWICK_FLAT_INDEX_MAX, when any element is not an accepted value
 * (VOID included: write a zero for a zero), or when a stored block sum
 * would be out of range, or on allocation failure. */
[[nodiscard]] multiFenwickFlat *
multiFenwickFlatNewFromArray(const databox *values, size_t count);

/* Free tree */
void multiFenwickFlatFree(multiFenwickFlat *mfw);

/* ====================================================================
 * CORE OPERATIONS
 * ==================================================================== */

/* Largest index Update and Set accept, the same as MULTIFENWICK_INDEX_MAX
 * so the two trees accept the same indexes.  Capacity stays at or below 2^30
 * cells (9 GiB of cells). */
#define MULTIFENWICK_FLAT_INDEX_MAX ((((uint64_t)1) << 30) - 1)

/* Update: add delta to element at idx
 * Time: O(log n), plus amortized O(1) growth past the capacity
 * mfw: pointer-to-pointer (created if NULL)
 * idx: 0-based index, at most MULTIFENWICK_FLAT_INDEX_MAX; past the end
 *      it extends the count to idx + 1 (the elements between are zero)
 * delta: value to add (negative to subtract)
 * Returns false, changing nothing (a NULL '*mfw' stays NULL), for a NULL
 * 'mfw' or 'delta', a delta that is not an accepted value, an index above
 * MULTIFENWICK_FLAT_INDEX_MAX, a stored block sum that would go out of
 * range, or a failed allocation of a new tree. */
bool multiFenwickFlatUpdate(multiFenwickFlat **mfw, size_t idx,
                            const databox *delta);

/* Query: compute prefix sum [0, idx]
 * Time: O(log n)
 * Returns false for a NULL tree or result, idx >= count, or a sum out of
 * range; a non-NULL result is then VOID. */
bool multiFenwickFlatQuery(const multiFenwickFlat *mfw, size_t idx,
                           databox *result);

/* Range query: compute sum [left, right] (inclusive), exactly, from the
 * two prefix sums around it
 * Time: O(log n)
 * Returns false for a NULL tree or result, left > right, right >= count,
 * or a sum out of range; a non-NULL result is then VOID. */
bool multiFenwickFlatRangeQuery(const multiFenwickFlat *mfw, size_t left,
                                size_t right, databox *result);

/* Get single element value at idx
 * Time: O(log n), from the cells covering the element alone
 * Returns false for a NULL tree or result, idx >= count, or a value out
 * of range; a non-NULL result is then VOID. */
bool multiFenwickFlatGet(const multiFenwickFlat *mfw, size_t idx,
                         databox *result);

/* Set single element to exact value (not delta)
 * Time: O(log n)
 * One transactional Update by value - current, with that difference kept
 * exact.  Returns false, changing nothing, for the same reasons as
 * Update. */
bool multiFenwickFlatSet(multiFenwickFlat **mfw, size_t idx,
                         const databox *value);

/* ====================================================================
 * METADATA & INSPECTION
 * ==================================================================== */

/* Get number of elements */
size_t multiFenwickFlatCount(const multiFenwickFlat *mfw);

/* Get total bytes used: the struct plus 8 bytes per allocated cell, 9 once
 * the kinds array exists */
size_t multiFenwickFlatBytes(const multiFenwickFlat *mfw);

/* ====================================================================
 * ADVANCED OPERATIONS
 * ==================================================================== */

/* Find smallest index with cumulative sum >= target
 * Time: O(log n) (binary lifting down the tree's cells)
 * Same contract, algorithm and results as multiFenwickLowerBound,
 * negative elements included. */
size_t multiFenwickFlatLowerBound(const multiFenwickFlat *mfw,
                                  const databox *target);

/* Reset all values to zero; the count and capacity stay as they were
 * Time: O(n) */
void multiFenwickFlatClear(multiFenwickFlat *mfw);

/* Persistence (persist.h), as multiFenwick's: snapshots keep the
 * capacity, the count and every cell; WAL NUM_INCR [index, delta]
 * (Update), UPDATE [index, value] (Set), CLEAR. */
extern const struct persistOps persistOpsMultiFenwickFlat;

/* ====================================================================
 * TESTING & DEBUG
 * ==================================================================== */

#ifdef DATAKIT_TEST
/* Print debug representation */
void multiFenwickFlatRepr(const multiFenwickFlat *mfw);

/* Run tests */
int multiFenwickFlatTest(int argc, char *argv[]);
#endif

/*
 * Flat multiFenwick Tree (Databox-Based Binary Indexed Tree)
 * Based on Fenwick Tree algorithm by Peter M. Fenwick (1994)
 */
