#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * offsetArray - Sparse Array with Automatic Offset Adjustment
 * ============================================================================
 * Use case:
 *   You want an array indexed by integers that don't start at zero.
 *   For example, file descriptors typically start around 3-5 and grow upward,
 *   or you have IDs starting at 1000. Rather than waste memory on unused
 *   lower indices, offsetArray automatically adjusts indices so the internal
 *   array starts at your lowest index.
 *
 * Features:
 *   - O(1) access after initial grow
 *   - Automatic offset adjustment (one subtraction per access)
 *   - Bidirectional growth (can grow both up and down)
 *   - Type-safe via macro-generated structs
 *
 * Limitations:
 *   - Not thread-safe
 *   - Downward growth requires O(n) memmove
 *   - Memory is NOT zero-initialized (caller must init after Grow)
 *   - No bounds checking in release mode
 *
 * Usage:
 *   // Create a type for storing MyStruct indexed by int
 *   offsetArrayCreateTypes(MyData, MyStruct, int);
 *
 *   offsetArrayMyData arr = {0};  // Zero-init required!
 *
 *   offsetArrayGrow(&arr, 100);   // Prepare to use index 100
 *   offsetArrayGet(&arr, 100) = myValue;  // Store at index 100
 *
 *   // Access (must have called Grow first!)
 *   MyStruct val = offsetArrayGet(&arr, 100);
 *
 *   offsetArrayFree(&arr);  // Cleanup
 * ============================================================================
 */

/* ----------------------------------------------------------------------------
 * Type Generation
 * ----------------------------------------------------------------------------
 * Creates a struct type: offsetArray##name
 *   - obj:     pointer to element storage
 *   - offset:  lowest valid index (subtracted on access)
 *   - highest: highest valid index
 *
 * Parameters:
 *   name:    suffix for generated type name
 *   storage: element type to store
 *   scale:   integer type for indices (int, size_t, int64_t, etc.)
 */
#define offsetArrayCreateTypes(name, storage, scale)                           \
    typedef struct offsetArray##name {                                         \
        storage *obj;                                                          \
        scale offset;                                                          \
        scale highest;                                                         \
    } offsetArray##name

/* ----------------------------------------------------------------------------
 * Internal Helpers (do not use directly)
 * --------------------------------------------------------------------------*/

/* Convert external index to internal array index */
#define offsetArrayAdjusted_(arr, idx) ((idx) - (arr)->offset)

/* Current allocated count (internal) */
#define offsetArrayAllocated_(arr)                                             \
    ((arr)->obj ? (size_t)((arr)->highest - (arr)->offset + 1) : 0)

/* Resize array to hold 'newCount' elements */
#define offsetArrayResize_(arr, newCount)                                      \
    do {                                                                       \
        /* @own: container; free=zfree */                                      \
        (arr)->obj = zrealloc((arr)->obj, (newCount) * sizeof(*(arr)->obj));   \
    } while (0)

/* ----------------------------------------------------------------------------
 * Core Operations
 * --------------------------------------------------------------------------*/

/* Check if array is empty (never been grown) */
#define offsetArrayEmpty(arr) ((arr)->obj == NULL)

/* Get count of addressable elements (0 if empty) */
#define offsetArrayCount(arr)                                                  \
    ((arr)->obj ? (size_t)((arr)->highest - (arr)->offset + 1) : 0)

/* Get lowest valid index */
#define offsetArrayLow(arr) ((arr)->offset)

/* Get highest valid index */
#define offsetArrayHigh(arr) ((arr)->highest)

/* Check if index is within current bounds (does NOT check if array is empty) */
#define offsetArrayContains(arr, idx)                                          \
    ((idx) >= (arr)->offset && (idx) <= (arr)->highest)

/* Grow array to include index 'idx'.
 * After this call, offsetArrayGet(arr, idx) is valid.
 * NOTE: New memory is NOT zero-initialized! */
#define offsetArrayGrow(arr, idx)                                              \
    do {                                                                       \
        if (!(arr)->obj) {                                                     \
            /* First allocation */                                             \
            (arr)->offset = (idx);                                             \
            (arr)->highest = (idx);                                            \
            offsetArrayResize_(arr, 1);                                        \
        } else if ((idx) < (arr)->offset) {                                    \
            /* Grow downward - need memmove */                                 \
            const size_t oldCount = offsetArrayAllocated_(arr);                \
            const size_t growBy = (size_t)((arr)->offset - (idx));             \
            const size_t newCount = oldCount + growBy;                         \
            offsetArrayResize_(arr, newCount);                                 \
            memmove((arr)->obj + growBy, (arr)->obj,                           \
                    oldCount * sizeof(*(arr)->obj));                           \
            (arr)->offset = (idx);                                             \
        } else if ((idx) > (arr)->highest) {                                   \
            /* Grow upward */                                                  \
            const size_t newCount = (size_t)((idx) - (arr)->offset + 1);       \
            offsetArrayResize_(arr, newCount);                                 \
            (arr)->highest = (idx);                                            \
        }                                                                      \
        /* else: idx already in range, no-op */                                \
    } while (0)

/* Grow and zero-initialize new element at idx.
 * Bounds are kept in the scale's own (possibly signed) arithmetic:
 * casting to size_t would scramble comparisons and memset spans for
 * negative-capable scale types. */
#define offsetArrayGrowZero(arr, idx)                                          \
    do {                                                                       \
        const bool wasEmpty_ = offsetArrayEmpty(arr);                          \
        const __typeof__((arr)->offset) oldLow_ =                              \
            wasEmpty_ ? 0 : (arr)->offset;                                     \
        const __typeof__((arr)->highest) oldHigh_ =                            \
            wasEmpty_ ? 0 : (arr)->highest;                                    \
        offsetArrayGrow(arr, idx);                                             \
        if (wasEmpty_) {                                                       \
            memset(&(arr)->obj[0], 0, sizeof(*(arr)->obj));                    \
        } else if ((idx) < oldLow_) {                                          \
            /* Grew downward - zero new low elements */                        \
            memset(&(arr)->obj[0], 0,                                          \
                   (size_t)(oldLow_ - (idx)) * sizeof(*(arr)->obj));           \
        } else if ((idx) > oldHigh_) {                                         \
            /* Grew upward - zero new high elements */                         \
            const size_t firstNew = (size_t)(oldHigh_ - (arr)->offset) + 1;    \
            memset(&(arr)->obj[firstNew], 0,                                   \
                   (size_t)((idx) - oldHigh_) * sizeof(*(arr)->obj));          \
        }                                                                      \
    } while (0)

/* Access element at index 'idx'.
 * PRECONDITION: offsetArrayGrow(arr, idx) must have been called first!
 * Returns an lvalue - can be used for both read and write. */
#define offsetArrayGet(arr, idx) ((arr)->obj[offsetArrayAdjusted_(arr, idx)])

/* Access element by internal zero-based index (for iteration).
 * Valid indices: 0 to offsetArrayCount(arr)-1 */
#define offsetArrayDirect(arr, zeroIdx) ((arr)->obj[zeroIdx])

/* Free array and reset to empty state */
#define offsetArrayFree(arr)                                                   \
    do {                                                                       \
        zfree((arr)->obj);                                                     \
        (arr)->obj = NULL;                                                     \
        (arr)->offset = 0;                                                     \
        (arr)->highest = 0;                                                    \
    } while (0)

/* ----------------------------------------------------------------------------
 * Debug Bounds Checking (enabled with OFFSETARRAY_DEBUG)
 * ----------------------------------------------------------------------------
 * offsetArrayGetChecked_ is offsetArrayGet with an aborting bounds check.  It
 * is an lvalue like offsetArrayGet (assign through it, take its address), and
 * evaluates 'arr' and 'idx' once each.  Building with -DOFFSETARRAY_DEBUG
 * makes offsetArrayGet this form.  It is always defined so the offsetArray
 * tests exercise it in every build.
 * --------------------------------------------------------------------------*/
#include "dkDiag.h"
#include <inttypes.h>
#include <stdint.h>

#define offsetArrayGetChecked_(arr, idx)                                       \
    (*({                                                                       \
        __typeof__(arr) arrC_ = (arr);                                         \
        /* the type offsetArrayGet's (idx) - offset is computed in */          \
        const __typeof__((idx) - (arr)->offset) idxC_ = (idx);                 \
        offsetArrayBoundsCheck_(arrC_->obj != NULL, idxC_ >= arrC_->offset,    \
                                idxC_ <= arrC_->highest, #arr, (int64_t)idxC_, \
                                __FILE__, __LINE__);                           \
        &arrC_->obj[offsetArrayAdjusted_(arrC_, idxC_)];                       \
    }))

#ifdef OFFSETARRAY_DEBUG
#undef offsetArrayGet
#define offsetArrayGet(arr, idx) offsetArrayGetChecked_(arr, idx)
#endif

/* A failed check is a FATAL DK_DIAG_OFFSETARRAY_BOUNDS diagnostic, located
 * at the offsetArrayGet that failed, then abort() */
static inline void offsetArrayBoundsCheck_(bool hasObj, bool aboveLow,
                                           bool belowHigh, const char *name,
                                           int64_t idx, const char *file,
                                           int line) {
    if (!hasObj) {
        dkDiagFatal_(DK_DIAG_OFFSETARRAY_BOUNDS, "offsetArrayGet", file, line,
                     "offsetArrayGet(%s, %" PRId64 "): array is empty", name,
                     idx);
    }
    if (!aboveLow || !belowHigh) {
        dkDiagFatal_(DK_DIAG_OFFSETARRAY_BOUNDS, "offsetArrayGet", file, line,
                     "offsetArrayGet(%s, %" PRId64 "): index out of bounds",
                     name, idx);
    }
}

#ifdef DATAKIT_TEST
int offsetArrayTest(int argc, char *argv[]);
#endif
