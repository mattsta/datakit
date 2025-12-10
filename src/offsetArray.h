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

/* Grow and zero-initialize new element at idx */
#define offsetArrayGrowZero(arr, idx)                                          \
    do {                                                                       \
        const bool wasEmpty_ = offsetArrayEmpty(arr);                          \
        const size_t oldLow_ = wasEmpty_ ? 0 : (arr)->offset;                  \
        const size_t oldHigh_ = wasEmpty_ ? 0 : (arr)->highest;                \
        offsetArrayGrow(arr, idx);                                             \
        if (wasEmpty_) {                                                       \
            memset(&(arr)->obj[0], 0, sizeof(*(arr)->obj));                    \
        } else if ((size_t)(idx) < oldLow_) {                                  \
            /* Grew downward - zero new low elements */                        \
            memset(&(arr)->obj[0], 0,                                          \
                   (oldLow_ - (idx)) * sizeof(*(arr)->obj));                   \
        } else if ((size_t)(idx) > oldHigh_) {                                 \
            /* Grew upward - zero new high elements */                         \
            const size_t firstNew = oldHigh_ - (arr)->offset + 1;              \
            memset(&(arr)->obj[firstNew], 0,                                   \
                   ((idx) - oldHigh_) * sizeof(*(arr)->obj));                  \
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
 * --------------------------------------------------------------------------*/
#ifdef OFFSETARRAY_DEBUG
#include <stdio.h>
#include <stdlib.h>

#undef offsetArrayGet
#define offsetArrayGet(arr, idx)                                               \
    (offsetArrayBoundsCheck_((arr)->obj != NULL, (idx) >= (arr)->offset,       \
                             (idx) <= (arr)->highest, #arr, idx, __FILE__,     \
                             __LINE__),                                        \
     (arr)->obj[offsetArrayAdjusted_(arr, idx)])

static inline void offsetArrayBoundsCheck_(bool hasObj, bool aboveLow,
                                           bool belowHigh, const char *name,
                                           long long idx, const char *file,
                                           int line) {
    if (!hasObj) {
        fprintf(stderr, "%s:%d: offsetArrayGet(%s, %lld): array is empty\n",
                file, line, name, idx);
        abort();
    }
    if (!aboveLow || !belowHigh) {
        fprintf(stderr,
                "%s:%d: offsetArrayGet(%s, %lld): index out of bounds\n", file,
                line, name, idx);
        abort();
    }
}
#endif

#ifdef DATAKIT_TEST
int offsetArrayTest(int argc, char *argv[]);
#endif
