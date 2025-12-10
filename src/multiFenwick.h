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
 * - No datatype decay - maintains precision of chosen type
 * - Uses multilist for efficient storage
 * - Same O(log n) performance as standard fenwick
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
 * TYPE CONSISTENCY
 * ----------------
 * All databox values should use the same numeric type for consistent
 * behavior. Mixing types (e.g., int64_t and double) is supported but
 * may produce unexpected results due to type coercion.
 *
 * LIMITATIONS
 * -----------
 * - Does not support range updates directly
 * - Does not support minimum/maximum queries (use segment tree)
 * - Requires non-negative indices
 * - All values must be numeric databox types
 */

/* Opaque multiFenwick tree type */
typedef struct multiFenwick multiFenwick;

/* ====================================================================
 * CREATION & DESTRUCTION
 * ==================================================================== */

/* Create new empty multiFenwick tree */
multiFenwick *multiFenwickNew(void);

/* Create multiFenwick tree from databox array
 * Time: O(n log n) via repeated updates
 * Returns NULL on allocation failure
 * All databoxes should be the same numeric type */
multiFenwick *multiFenwickNewFromArray(const databox *values, size_t count);

/* Free multiFenwick tree */
void multiFenwickFree(multiFenwick *mfw);

/* ====================================================================
 * CORE OPERATIONS
 * ==================================================================== */

/* Update: add delta to element at idx
 * Time: O(log n)
 * mfw: pointer-to-pointer (may upgrade tier)
 * idx: 0-based index
 * delta: databox value to add (can be negative for signed types)
 * Returns true on success, false on allocation failure or invalid type */
bool multiFenwickUpdate(multiFenwick **mfw, size_t idx, const databox *delta);

/* Query: compute prefix sum [0, idx]
 * Time: O(log n)
 * idx: 0-based index
 * result: output databox to store sum
 * Returns true on success, false if out of bounds
 * Result will be a VOID databox if tree is empty */
bool multiFenwickQuery(const multiFenwick *mfw, size_t idx, databox *result);

/* Range query: compute sum [left, right]
 * Time: O(log n)
 * left, right: 0-based indices (inclusive)
 * result: output databox to store sum
 * Returns true on success
 * Returns VOID databox if left > right */
bool multiFenwickRangeQuery(const multiFenwick *mfw, size_t left, size_t right,
                            databox *result);

/* Get single element value at idx
 * Time: O(log n)
 * Computes: query(idx) - query(idx-1)
 * result: output databox to store value
 * Returns true on success, false if out of bounds */
bool multiFenwickGet(const multiFenwick *mfw, size_t idx, databox *result);

/* Set single element to exact value (not delta)
 * Time: O(log n)
 * Computes delta = value - get(idx), then updates
 * Returns true on success */
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
 * Time: O(log^2 n) naive, O(log n) with binary lifting
 * Returns SIZE_MAX if no such index exists or tree is empty
 * Requires non-negative values for correctness
 * Comparison uses databox comparison logic */
size_t multiFenwickLowerBound(const multiFenwick *mfw, const databox *target);

/* Reset all values to zero (maintains capacity)
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
 *
 * Databox implementation:
 * Copyright (c) 2024-2025, Matt Stancliff
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
