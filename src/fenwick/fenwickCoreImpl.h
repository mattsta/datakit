/* ====================================================================
 * Fenwick Tree Core Implementation Macros (2-TIER SYSTEM)
 * ====================================================================
 *
 * This file contains the actual implementation of all Fenwick tree
 * operations as macros. It is included multiple times with different
 * type parameters to generate type-specific implementations.
 *
 * Architecture: Small (contiguous) → Full (unlimited)
 * - Small: Cache-friendly, 0 pointer indirection, ~20-33% faster
 * - Full: Unlimited growth, overflow protection, uint64_t indexes
 *
 * REQUIRED DEFINES (must be set before including this file):
 *   FENWICK_SUFFIX - Type suffix (I64, U32, Double, etc.)
 *   FENWICK_VALUE_TYPE - C type for values
 *   FENWICK_INDEX_TYPE_SMALL - Index type for Small tier (uint32_t)
 *   FENWICK_INDEX_TYPE_FULL - Index type for Full tier (uint64_t)
 *   FENWICK_SMALL_MAX_COUNT - Max count for Small tier before upgrade
 *   FENWICK_IS_SIGNED - 1 if signed, 0 if unsigned/float
 *   FENWICK_IS_FLOATING - 1 if floating-point, 0 if integer
 *   FENWICK_IMPL_SCOPE - Either 'static inline' or '' (empty for .c file)
 *
 * EXAMPLE:
 *   #define FENWICK_SUFFIX I64
 *   #define FENWICK_VALUE_TYPE int64_t
 *   #define FENWICK_INDEX_TYPE_SMALL uint32_t
 *   #define FENWICK_INDEX_TYPE_FULL uint64_t
 *   #define FENWICK_SMALL_MAX_COUNT (8 * 1024)
 *   #define FENWICK_MEDIUM_MAX_COUNT (1 * 1024 * 1024)
 *   #define FENWICK_IS_SIGNED 1
 *   #define FENWICK_IS_FLOATING 0
 *   #define FENWICK_IMPL_SCOPE
 *   #include "fenwickCoreImpl.h"
 */

#ifndef FENWICK_SUFFIX
#error "FENWICK_SUFFIX must be defined before including fenwickCoreImpl.h"
#endif

#ifndef FENWICK_VALUE_TYPE
#error "FENWICK_VALUE_TYPE must be defined before including fenwickCoreImpl.h"
#endif

/* Set defaults for optional parameters */
#ifndef FENWICK_INDEX_TYPE_SMALL
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#endif

#ifndef FENWICK_INDEX_TYPE_FULL
#define FENWICK_INDEX_TYPE_FULL uint64_t
#endif

#ifndef FENWICK_IS_SIGNED
#define FENWICK_IS_SIGNED 1
#endif

#ifndef FENWICK_IS_FLOATING
#define FENWICK_IS_FLOATING 0
#endif

#ifndef FENWICK_IMPL_SCOPE
#define FENWICK_IMPL_SCOPE
#endif

/* Calculate tier thresholds based on value size if not provided */
#ifndef FENWICK_SMALL_MAX_COUNT
#define FENWICK_SMALL_MAX_COUNT ((128 * 1024) / sizeof(FENWICK_VALUE_TYPE))
#endif

#ifndef FENWICK_MEDIUM_MAX_COUNT
#define FENWICK_MEDIUM_MAX_COUNT                                               \
    ((16 * 1024 * 1024) / sizeof(FENWICK_VALUE_TYPE))
#endif

#ifndef FENWICK_SMALL_MAX_BYTES
#define FENWICK_SMALL_MAX_BYTES (128 * 1024)
#endif

#ifndef FENWICK_MEDIUM_MAX_BYTES
#define FENWICK_MEDIUM_MAX_BYTES (16 * 1024 * 1024)
#endif

/* ====================================================================
 * SMALL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new small Fenwick tree */
FENWICK_IMPL_SCOPE FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX, SmallNew)(void) {
    FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *fw =
        zcalloc(1, sizeof(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)));
    fw->count = 0;
    fw->capacity = 0;
    return fw;
}

/* Create from array */
FENWICK_IMPL_SCOPE FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 SmallNewFromArray)(const FENWICK_VALUE_TYPE *values,
                                    FENWICK_INDEX_TYPE_SMALL count) {
    if (!values || count == 0) {
        return FENWICK_NAME(FENWICK_SUFFIX, SmallNew)();
    }

    /* Capacity must be next power of 2 > count */
    FENWICK_INDEX_TYPE_SMALL capacity = 1;
    while (capacity <= count) {
        capacity <<= 1;
    }

    size_t size = sizeof(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)) +
                  capacity * sizeof(FENWICK_VALUE_TYPE);
    FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *fw = zcalloc(1, size);
    fw->count = count;
    fw->capacity = capacity;

    /* Build BIT by applying updates */
    for (FENWICK_INDEX_TYPE_SMALL i = 0; i < count; i++) {
        FENWICK_VALUE_TYPE value = values[i];
        if (FENWICK_IS_FLOATING) {
            if (value == (FENWICK_VALUE_TYPE)0.0) {
                continue;
            }
        } else {
            if (value == (FENWICK_VALUE_TYPE)0) {
                continue;
            }
        }

        FENWICK_INDEX_TYPE_SMALL idx = i + 1;
        while (idx <= capacity) {
            fw->tree[idx - 1] += value;
            idx = fenwickParent(idx);
        }
    }

    return fw;
}

/* Free small Fenwick tree */
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             SmallFree)(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (fw) {
        zfree(fw);
    }
}

/* Update: add delta to element at idx */
FENWICK_IMPL_SCOPE FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 SmallUpdate)(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
                              FENWICK_INDEX_TYPE_SMALL idx,
                              FENWICK_VALUE_TYPE delta, bool *success) {
    if (!fw) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    /* Ensure capacity */
    if (idx >= fw->capacity) {
        FENWICK_INDEX_TYPE_SMALL oldCapacity = fw->capacity;
        FENWICK_INDEX_TYPE_SMALL newCount = idx + 1;
        FENWICK_INDEX_TYPE_SMALL newCapacity = 1;

        /* Find next power of 2 > newCount */
        while (newCapacity <= newCount) {
            newCapacity <<= 1;
        }

        size_t newSize = sizeof(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)) +
                         newCapacity * sizeof(FENWICK_VALUE_TYPE);

        FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *newFw = zrealloc(fw, newSize);
        if (!newFw) {
            if (success) {
                *success = false;
            }
            return fw;
        }

        /* Zero-initialize new elements */
        memset(&newFw->tree[newFw->capacity], 0,
               (newCapacity - newFw->capacity) * sizeof(FENWICK_VALUE_TYPE));

        FENWICK_INDEX_TYPE_SMALL oldCount = newFw->count;
        newFw->count = newCount;
        newFw->capacity = newCapacity;
        fw = newFw;

        /* Retroactively propagate existing values to new parent nodes */
        if (oldCapacity > 0 && oldCount > 0) {
            for (FENWICK_INDEX_TYPE_SMALL pos = 1; pos <= oldCount; pos++) {
                FENWICK_VALUE_TYPE value;
                if (pos == 1) {
                    value = fw->tree[0];
                } else {
                    /* value = query(pos) - query(pos-1) */
                    FENWICK_VALUE_TYPE sum_pos =
                        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
                    FENWICK_VALUE_TYPE sum_prev =
                        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);

                    FENWICK_INDEX_TYPE_SMALL p = pos;
                    while (p > 0 && p <= oldCapacity) {
                        sum_pos += fw->tree[p - 1];
                        p -= (p & -p);
                    }

                    p = pos - 1;
                    while (p > 0 && p <= oldCapacity) {
                        sum_prev += fw->tree[p - 1];
                        p -= (p & -p);
                    }

                    value = sum_pos - sum_prev;
                }

                /* Propagate to new parents */
                FENWICK_INDEX_TYPE_SMALL parent = pos + (pos & -pos);
                while (parent <= newCapacity) {
                    if (parent > oldCapacity) {
                        fw->tree[parent - 1] += value;
                    }
                    parent += (parent & -parent);
                }
            }
        }
    } else if (idx >= fw->count) {
        fw->count = idx + 1;
    }

    /* BIT update: traverse upward adding LSB */
    FENWICK_INDEX_TYPE_SMALL i = idx + 1;
    while (i <= fw->capacity) {
        fw->tree[i - 1] += delta;
        i = fenwickParent(i);
    }

    if (success) {
        *success = true;
    }
    return fw;
}

/* Query: compute prefix sum [0, idx] */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX, SmallQuery)(
    const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_SMALL idx) {
    if (!fw || idx >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE sum =
        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    FENWICK_INDEX_TYPE_SMALL i = idx + 1;

    while (i > 0) {
        sum += fw->tree[i - 1];
        i = fenwickPrev(i);
    }

    return sum;
}

/* Range query: sum [left, right] */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX,
                                                   SmallRangeQuery)(
    const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_SMALL left, FENWICK_INDEX_TYPE_SMALL right) {
    if (!fw || left > right || right >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE rightSum =
        FENWICK_NAME(FENWICK_SUFFIX, SmallQuery)(fw, right);

    if (left == 0) {
        return rightSum;
    }

    FENWICK_VALUE_TYPE leftSum =
        FENWICK_NAME(FENWICK_SUFFIX, SmallQuery)(fw, left - 1);
    return rightSum - leftSum;
}

/* Get single element value */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX, SmallGet)(
    const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_SMALL idx) {
    if (!fw || idx >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE current =
        FENWICK_NAME(FENWICK_SUFFIX, SmallQuery)(fw, idx);

    if (idx == 0) {
        return current;
    }

    FENWICK_VALUE_TYPE previous =
        FENWICK_NAME(FENWICK_SUFFIX, SmallQuery)(fw, idx - 1);
    return current - previous;
}

/* Set single element to exact value */
FENWICK_IMPL_SCOPE FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 SmallSet)(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
                           FENWICK_INDEX_TYPE_SMALL idx,
                           FENWICK_VALUE_TYPE value, bool *success) {
    if (!fw) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    FENWICK_VALUE_TYPE current =
        FENWICK_NAME(FENWICK_SUFFIX, SmallGet)(fw, idx);
    FENWICK_VALUE_TYPE delta = value - current;

    return FENWICK_NAME(FENWICK_SUFFIX, SmallUpdate)(fw, idx, delta, success);
}

/* Get element count */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_SMALL
FENWICK_NAME(FENWICK_SUFFIX,
             SmallCount)(const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    return fw ? fw->count : 0;
}

/* Get total bytes used */
FENWICK_IMPL_SCOPE size_t FENWICK_NAME(FENWICK_SUFFIX, SmallBytes)(
    const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        return 0;
    }
    return sizeof(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)) +
           fw->capacity * sizeof(FENWICK_VALUE_TYPE);
}

/* Check if should upgrade to medium tier */
FENWICK_IMPL_SCOPE bool FENWICK_NAME(FENWICK_SUFFIX, SmallShouldUpgrade)(
    const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        return false;
    }

    if (fw->count > FENWICK_SMALL_MAX_COUNT) {
        return true;
    }

    if (FENWICK_NAME(FENWICK_SUFFIX, SmallBytes)(fw) >
        FENWICK_SMALL_MAX_BYTES) {
        return true;
    }

    return false;
}

/* Find smallest index with cumulative sum >= target */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_SMALL
FENWICK_NAME(FENWICK_SUFFIX,
             SmallLowerBound)(const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw,
                              FENWICK_VALUE_TYPE target) {
    if (!fw || fw->count == 0) {
        return (FENWICK_INDEX_TYPE_SMALL)-1;
    }

    /* Find highest power of 2 <= count */
    FENWICK_INDEX_TYPE_SMALL pos = 0;
    FENWICK_INDEX_TYPE_SMALL bitMask = 1;
    while (bitMask <= fw->count) {
        bitMask <<= 1;
    }
    bitMask >>= 1;

    FENWICK_VALUE_TYPE currentSum =
        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);

    /* Binary lifting */
    while (bitMask > 0) {
        FENWICK_INDEX_TYPE_SMALL nextPos = pos + bitMask;

        if (nextPos <= fw->count &&
            currentSum + fw->tree[nextPos - 1] < target) {
            pos = nextPos;
            currentSum += fw->tree[pos - 1];
        }

        bitMask >>= 1;
    }

    if (pos < fw->count) {
        return pos;
    }

    return (FENWICK_INDEX_TYPE_SMALL)-1;
}

/* Clear all values to zero */
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             SmallClear)(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        return;
    }

    memset(fw->tree, 0, fw->capacity * sizeof(FENWICK_VALUE_TYPE));
}

#ifdef DATAKIT_TEST
/* Print debug representation */
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             SmallRepr)(const FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        printf("FenwickSmall: (nil)\n");
        return;
    }

    printf("FenwickSmall [count=%u, capacity=%u, bytes=%zu]\n",
           (unsigned)fw->count, (unsigned)fw->capacity,
           FENWICK_NAME(FENWICK_SUFFIX, SmallBytes)(fw));
}
#endif

/* Note: Full tier implementation is in fenwickCoreFullImpl.h
 * Small tier is optimized for cache-locality with contiguous allocation.
 * Full tier provides unlimited growth with overflow protection. */

/* Clean up defines for next instantiation */
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_INDEX_TYPE_SMALL
#undef FENWICK_INDEX_TYPE_FULL
#undef FENWICK_SMALL_MAX_COUNT
#undef FENWICK_SMALL_MAX_BYTES
#undef FENWICK_IS_SIGNED
#undef FENWICK_IS_FLOATING
#undef FENWICK_IMPL_SCOPE
