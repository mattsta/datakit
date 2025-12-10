#include "multiFenwick.h"
#include "datakit.h"
#include "flexCapacityManagement.h"
#include "mflexInternal.h"
#include "multiFenwickCommon.h"
#include "multilist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* multiFenwick structure - uses multilist for storage
 * Multilist handles all tiering and memory management automatically */
struct multiFenwick {
    multilist *tree;   /* BIT values stored as flex components in multilist */
    uint64_t count;    /* Logical count: highest index + 1 */
    uint64_t capacity; /* Allocated size (power-of-2, for BIT traversal) */
    mflexState *state; /* State for multilist operations */
};

/* Create new multiFenwick tree */
multiFenwick *multiFenwickNew(void) {
    multiFenwick *mfw = zcalloc(1, sizeof(*mfw));
    if (!mfw) {
        return NULL;
    }

    /* Create multilist with reasonable limits */
    /* Tradeoff: the larger the multilist flex:
     *   - more memory saved for longer flex compaction
     *   - slower speed because it requires more linear updates */
    mfw->tree = multilistNew(FLEX_CAP_LEVEL_8192, 0);
    if (!mfw->tree) {
        zfree(mfw);
        return NULL;
    }

    mfw->count = 0;
    mfw->capacity = 0;

    /* Allocate state */
    mfw->state = zcalloc(1, sizeof(mflexState));
    if (!mfw->state) {
        multilistFree(mfw->tree);
        zfree(mfw);
        return NULL;
    }

    return mfw;
}

/* Create from array of databoxes */
multiFenwick *multiFenwickNewFromArray(const databox *values, size_t count) {
    if (!values || count == 0) {
        return multiFenwickNew();
    }

    multiFenwick *mfw = multiFenwickNew();
    if (!mfw) {
        return NULL;
    }

    /* Capacity must be next power of 2 > count */
    uint64_t capacity = 1;
    while (capacity <= count) {
        capacity <<= 1;
    }

    /* Pre-populate multilist with zero values up to capacity */
    databox zero = databoxZeroLike(&values[0]);
    for (uint64_t i = 0; i < capacity; i++) {
        multilistPushByTypeTail(&mfw->tree, mfw->state, &zero);
    }

    mfw->count = count;
    mfw->capacity = capacity;

    /* Build BIT by applying updates for each value
     * Time: O(n log n) */
    for (uint64_t i = 0; i < count; i++) {
        if (DATABOX_IS_VOID(&values[i])) {
            continue; /* Skip zero/void values */
        }

        /* Check if value is zero - optimization */
        databox zeroCheck = databoxZeroLike(&values[i]);
        if (databoxCompareNumeric(&values[i], &zeroCheck) == 0) {
            continue;
        }

        /* Update: add value to position i (0-based external, 1-based internal)
         */
        uint64_t idx = i + 1; /* Convert to 1-based */
        while (idx <= capacity) {
            /* Get current value at BIT position idx */
            multilistEntry entry;
            if (multilistIndex(mfw->tree, mfw->state, idx - 1, &entry, true)) {
                /* Add values[i] to current BIT value */
                databox newVal;
                if (!databoxAdd(&entry.box, &values[i], &newVal)) {
                    /* Type mismatch or error */
                    multiFenwickFree(mfw);
                    return NULL;
                }

                /* Update the multilist entry */
                if (!multilistReplaceByTypeAtIndex(&mfw->tree, mfw->state,
                                                   idx - 1, &newVal)) {
                    multiFenwickFree(mfw);
                    return NULL;
                }
            }

            idx = multiFenwickParent(idx);
        }
    }

    return mfw;
}

/* Free multiFenwick tree */
void multiFenwickFree(multiFenwick *mfw) {
    if (!mfw) {
        return;
    }

    if (mfw->tree) {
        multilistFree(mfw->tree);
    }

    if (mfw->state) {
        zfree(mfw->state);
    }

    zfree(mfw);
}

/* Ensure capacity for given index
 * Returns true on success */
static bool multiFenwickEnsureCapacity(multiFenwick *mfw, uint64_t idx) {
    if (!mfw) {
        return false;
    }

    uint64_t newCount = idx + 1;

    /* Check if we need to grow */
    if (idx < mfw->capacity) {
        /* Have capacity, just update logical count */
        if (newCount > mfw->count) {
            mfw->count = newCount;
        }
        return true;
    }

    /* Need to grow capacity */
    uint64_t oldCapacity = mfw->capacity;
    uint64_t newCapacity = (oldCapacity == 0) ? 1 : oldCapacity;

    /* Find next power of 2 > newCount */
    while (newCapacity <= newCount) {
        newCapacity <<= 1;
    }

    /* Get a zero value of the appropriate type
     * If tree is empty, we can't infer type yet, so use SIGNED_64 as default.
     * Type coercion in databoxAdd will handle mismatches. */
    databox zero = DATABOX_SIGNED(0);

    /* Add zero elements to expand capacity */
    for (uint64_t i = oldCapacity; i < newCapacity; i++) {
        multilistPushByTypeTail(&mfw->tree, mfw->state, &zero);
    }

    /* Update metadata */
    mfw->count = newCount;
    mfw->capacity = newCapacity;

    /* CRITICAL: Retroactively propagate existing values to new parent nodes
     * When capacity grows, old values haven't updated the new parent positions
     */
    if (oldCapacity > 0) {
        uint64_t oldCount = (newCount < oldCapacity) ? newCount : oldCapacity;

        for (uint64_t pos = 1; pos <= oldCount; pos++) {
            /* Get the actual value at this position (not the BIT value) */
            databox value;

            /* Compute value = query(pos) - query(pos-1) */
            if (pos == 1) {
                /* First element is just query(0) */
                multilistEntry entry;
                if (multilistIndex(mfw->tree, mfw->state, 0, &entry, true)) {
                    value = entry.box;
                } else {
                    continue;
                }
            } else {
                /* value = query(pos) - query(pos-1) */
                databox sumPos = databoxZeroLike(&zero);
                databox sumPrev = databoxZeroLike(&zero);

                /* Compute query(pos) */
                uint64_t p = pos;
                while (p > 0 && p <= oldCapacity) {
                    multilistEntry entry;
                    if (multilistIndex(mfw->tree, mfw->state, p - 1, &entry,
                                       true)) {
                        databox temp;
                        if (!databoxAdd(&sumPos, &entry.box, &temp)) {
                            continue;
                        }
                        sumPos = temp;
                    }
                    p -= multiFenwickLSB(p);
                }

                /* Compute query(pos-1) */
                p = pos - 1;
                while (p > 0 && p <= oldCapacity) {
                    multilistEntry entry;
                    if (multilistIndex(mfw->tree, mfw->state, p - 1, &entry,
                                       true)) {
                        databox temp;
                        if (!databoxAdd(&sumPrev, &entry.box, &temp)) {
                            continue;
                        }
                        sumPrev = temp;
                    }
                    p -= multiFenwickLSB(p);
                }

                if (!databoxSubtract(&sumPos, &sumPrev, &value)) {
                    continue;
                }
            }

            /* Now propagate this value to parents in range (oldCapacity,
             * newCapacity] */
            uint64_t parent = pos + multiFenwickLSB(pos);
            while (parent <= newCapacity) {
                if (parent > oldCapacity) {
                    /* This parent is in the newly allocated range */
                    multilistEntry entry;
                    if (multilistIndex(mfw->tree, mfw->state, parent - 1,
                                       &entry, true)) {
                        databox newVal;
                        if (databoxAdd(&entry.box, &value, &newVal)) {
                            multilistReplaceByTypeAtIndex(
                                &mfw->tree, mfw->state, parent - 1, &newVal);
                        }
                    }
                }
                parent += multiFenwickLSB(parent);
            }
        }
    }

    return true;
}

/* Update: add delta to element at idx */
bool multiFenwickUpdate(multiFenwick **mfw, size_t idx, const databox *delta) {
    if (!mfw || !delta) {
        return false;
    }

    if (!*mfw) {
        *mfw = multiFenwickNew();
        if (!*mfw) {
            return false;
        }
    }

    multiFenwick *tree = *mfw;

    /* Ensure capacity */
    if (!multiFenwickEnsureCapacity(tree, idx)) {
        return false;
    }

    /* BIT update: traverse upward adding LSB
     * Convert to 1-based indexing internally */
    uint64_t i = idx + 1;
    while (i <= tree->capacity) {
        /* Get current value at BIT position i */
        multilistEntry entry;
        if (!multilistIndex(tree->tree, tree->state, i - 1, &entry, true)) {
            return false;
        }

        /* Add delta to current value
         * databoxAdd handles type coercion automatically */
        databox newVal;
        if (!databoxAdd(&entry.box, delta, &newVal)) {
            return false;
        }

        /* Update the multilist entry */
        if (!multilistReplaceByTypeAtIndex(&tree->tree, tree->state, i - 1,
                                           &newVal)) {
            return false;
        }

        i = multiFenwickParent(i);
    }

    return true;
}

/* Query: compute prefix sum [0, idx] */
bool multiFenwickQuery(const multiFenwick *mfw, size_t idx, databox *result) {
    if (!mfw || !result) {
        return false;
    }

    if (idx >= mfw->count) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    /* Get a zero value of the right type */
    multilistEntry entry;
    if (!multilistIndex(mfw->tree, mfw->state, 0, &entry, true)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    databox sum = databoxZeroLike(&entry.box);
    uint64_t i = idx + 1; /* Convert to 1-based */

    /* BIT query: traverse downward subtracting LSB */
    while (i > 0) {
        if (multilistIndex(mfw->tree, mfw->state, i - 1, &entry, true)) {
            databox temp;
            if (!databoxAdd(&sum, &entry.box, &temp)) {
                *result = DATABOX_BOX_VOID;
                return false;
            }
            sum = temp;
        }
        i = multiFenwickPrev(i);
    }

    *result = sum;
    return true;
}

/* Range query: sum [left, right] */
bool multiFenwickRangeQuery(const multiFenwick *mfw, size_t left, size_t right,
                            databox *result) {
    if (!mfw || !result) {
        return false;
    }

    if (left > right || right >= mfw->count) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    databox rightSum;
    if (!multiFenwickQuery(mfw, right, &rightSum)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    if (left == 0) {
        *result = rightSum;
        return true;
    }

    databox leftSum;
    if (!multiFenwickQuery(mfw, left - 1, &leftSum)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    if (!databoxSubtract(&rightSum, &leftSum, result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Get single element value */
bool multiFenwickGet(const multiFenwick *mfw, size_t idx, databox *result) {
    if (!mfw || !result) {
        return false;
    }

    if (idx >= mfw->count) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    databox current;
    if (!multiFenwickQuery(mfw, idx, &current)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    if (idx == 0) {
        *result = current;
        return true;
    }

    databox previous;
    if (!multiFenwickQuery(mfw, idx - 1, &previous)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    if (!databoxSubtract(&current, &previous, result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Set single element to exact value */
bool multiFenwickSet(multiFenwick **mfw, size_t idx, const databox *value) {
    if (!mfw || !value) {
        return false;
    }

    if (!*mfw) {
        *mfw = multiFenwickNew();
        if (!*mfw) {
            return false;
        }
    }

    /* Get current value */
    databox current;
    if (!multiFenwickGet(*mfw, idx, &current)) {
        /* Index out of bounds - just use zero */
        current = databoxZeroLike(value);
    }

    /* Compute delta = value - current */
    databox delta;
    if (!databoxSubtract(value, &current, &delta)) {
        return false;
    }

    return multiFenwickUpdate(mfw, idx, &delta);
}

/* Get count */
size_t multiFenwickCount(const multiFenwick *mfw) {
    return mfw ? mfw->count : 0;
}

/* Get bytes used */
size_t multiFenwickBytes(const multiFenwick *mfw) {
    if (!mfw) {
        return 0;
    }

    size_t total = sizeof(multiFenwick);
    total += sizeof(mflexState) * 2;

    if (mfw->tree) {
        total += multilistBytes(mfw->tree);
    }

    return total;
}

/* Lower bound search - O(log²n) implementation using binary search on queries
 * Simpler and more reliable than binary lifting on BIT values */
size_t multiFenwickLowerBound(const multiFenwick *mfw, const databox *target) {
    if (!mfw || !target || mfw->count == 0) {
        return SIZE_MAX;
    }

    /* Binary search for smallest index where prefix_sum >= target */
    size_t left = 0;
    size_t right = mfw->count - 1;
    size_t result = SIZE_MAX;

    while (left <= right) {
        size_t mid = left + (right - left) / 2;

        databox midSum;
        if (!multiFenwickQuery(mfw, mid, &midSum)) {
            break;
        }

        int cmp = databoxCompareNumeric(&midSum, target);

        if (cmp >= 0) {
            /* midSum >= target, so mid is a candidate */
            result = mid;
            /* Search left half for smaller index */
            if (mid == 0) {
                break;
            }
            right = mid - 1;
        } else {
            /* midSum < target, search right half */
            left = mid + 1;
        }
    }

    return result;
}

/* Clear all values to zero */
void multiFenwickClear(multiFenwick *mfw) {
    if (!mfw || !mfw->tree) {
        return;
    }

    /* Get zero value */
    databox zero = DATABOX_SIGNED(0);
    if (mfw->count > 0) {
        multilistEntry entry;
        if (multilistIndex(mfw->tree, mfw->state, 0, &entry, true)) {
            zero = databoxZeroLike(&entry.box);
        }
    }

    /* Set all values to zero */
    for (uint64_t i = 0; i < mfw->capacity; i++) {
        multilistReplaceByTypeAtIndex(&mfw->tree, mfw->state, i, &zero);
    }
}

#ifdef DATAKIT_TEST
/* Print debug representation */
void multiFenwickRepr(const multiFenwick *mfw) {
    if (!mfw) {
        printf("multiFenwick: (nil)\n");
        return;
    }

    size_t count = multiFenwickCount(mfw);
    size_t bytes = multiFenwickBytes(mfw);

    printf("multiFenwick [count=%zu, capacity=%zu, bytes=%zu]\n", count,
           (size_t)mfw->capacity, bytes);

    /* Print first few prefix sums */
    if (count > 0) {
        printf("  Prefix sums: [");
        size_t toShow = count < 20 ? count : 20;
        for (size_t i = 0; i < toShow; i++) {
            databox sum;
            if (multiFenwickQuery(mfw, i, &sum)) {
                switch (sum.type) {
                case DATABOX_SIGNED_64:
                    printf("%" PRId64, sum.data.i64);
                    break;
                case DATABOX_UNSIGNED_64:
                    printf("%" PRIu64, sum.data.u64);
                    break;
                case DATABOX_FLOAT_32:
                    printf("%.2f", (double)sum.data.f32);
                    break;
                case DATABOX_DOUBLE_64:
                    printf("%.2f", sum.data.d64);
                    break;
                default:
                    printf("?");
                    break;
                }
            } else {
                printf("?");
            }

            if (i < toShow - 1) {
                printf(", ");
            }
        }
        if (count > 20) {
            printf(", ... (%zu more)", count - 20);
        }
        printf("]\n");

        /* Print first few element values */
        printf("  Elements: [");
        for (size_t i = 0; i < toShow; i++) {
            databox val;
            if (multiFenwickGet(mfw, i, &val)) {
                switch (val.type) {
                case DATABOX_SIGNED_64:
                    printf("%" PRId64, val.data.i64);
                    break;
                case DATABOX_UNSIGNED_64:
                    printf("%" PRIu64, val.data.u64);
                    break;
                case DATABOX_FLOAT_32:
                    printf("%.2f", (double)val.data.f32);
                    break;
                case DATABOX_DOUBLE_64:
                    printf("%.2f", val.data.d64);
                    break;
                default:
                    printf("?");
                    break;
                }
            } else {
                printf("?");
            }

            if (i < toShow - 1) {
                printf(", ");
            }
        }
        if (count > 20) {
            printf(", ...");
        }
        printf("]\n");
    }
}
#endif
