/* ====================================================================
 * Fenwick Tree Full Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Full tier: {small_max}+ elements, unlimited growth.
 * Converts from Small tier in 2-tier architecture.
 * Generated via macro instantiation.
 *
 * REQUIRED DEFINES: Same as fenwickCoreImpl.h
 */

#ifndef FENWICK_SUFFIX
#error "FENWICK_SUFFIX must be defined before including fenwickCoreFullImpl.h"
#endif

#ifndef FENWICK_VALUE_TYPE
#error                                                                         \
    "FENWICK_VALUE_TYPE must be defined before including fenwickCoreFullImpl.h"
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

/* ====================================================================
 * FULL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new full Fenwick tree */
FENWICK_IMPL_SCOPE FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX, FullNew)(void) {
    FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *fw =
        zcalloc(1, sizeof(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)));
    fw->count = 0;
    fw->capacity = 0;
    fw->maxCapacity = (FENWICK_INDEX_TYPE_FULL)-1;
    fw->tree = NULL;
    return fw;
}

/* Create from array */
FENWICK_IMPL_SCOPE FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 FullNewFromArray)(const FENWICK_VALUE_TYPE *values,
                                   FENWICK_INDEX_TYPE_FULL count) {
    if (!values || count == 0) {
        return FENWICK_NAME(FENWICK_SUFFIX, FullNew)();
    }

    FENWICK_INDEX_TYPE_FULL capacity = 1;
    while (capacity <= count) {
        capacity <<= 1;
    }

    FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *fw =
        zcalloc(1, sizeof(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)));
    fw->count = count;
    fw->capacity = capacity;
    fw->maxCapacity = (FENWICK_INDEX_TYPE_FULL)-1;
    fw->tree = zcalloc(capacity, sizeof(FENWICK_VALUE_TYPE));

    /* Build BIT */
    for (FENWICK_INDEX_TYPE_FULL i = 0; i < count; i++) {
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

        FENWICK_INDEX_TYPE_FULL idx = i + 1;
        while (idx <= capacity) {
            fw->tree[idx - 1] += value;
            idx = fenwickParent(idx);
        }
    }

    return fw;
}

/* Convert from Small tier (2-TIER SYSTEM) */
FENWICK_IMPL_SCOPE FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 FullFromSmall)(FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) *
                                small) {
    if (!small) {
        return FENWICK_NAME(FENWICK_SUFFIX, FullNew)();
    }

    /* Extract all values from Small tier */
    FENWICK_INDEX_TYPE_SMALL count = small->count;
    FENWICK_VALUE_TYPE *values = zmalloc(count * sizeof(FENWICK_VALUE_TYPE));

    for (FENWICK_INDEX_TYPE_SMALL i = 0; i < count; i++) {
        values[i] = FENWICK_NAME(FENWICK_SUFFIX, SmallGet)(small, i);
    }

    /* Create Full from values */
    FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *full =
        FENWICK_NAME(FENWICK_SUFFIX, FullNewFromArray)(values, count);

    zfree(values);
    return full;
}

/* Free full Fenwick tree */
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             FullFree)(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (fw) {
        if (fw->tree) {
            zfree(fw->tree);
        }
        zfree(fw);
    }
}

/* Update with unlimited growth */
FENWICK_IMPL_SCOPE FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 FullUpdate)(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
                             FENWICK_INDEX_TYPE_FULL idx,
                             FENWICK_VALUE_TYPE delta, bool *success) {
    if (!fw) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    /* Ensure capacity */
    if (idx >= fw->capacity) {
        FENWICK_INDEX_TYPE_FULL oldCapacity = fw->capacity;
        FENWICK_INDEX_TYPE_FULL newCount = idx + 1;
        FENWICK_INDEX_TYPE_FULL newCapacity =
            (oldCapacity == 0) ? 1 : oldCapacity;

        while (newCapacity <= newCount) {
            /* Check for overflow */
            if (newCapacity > fw->maxCapacity / 2) {
                if (success) {
                    *success = false;
                }
                return fw;
            }
            newCapacity <<= 1;
        }

        FENWICK_VALUE_TYPE *newTree =
            zrealloc(fw->tree, newCapacity * sizeof(FENWICK_VALUE_TYPE));
        if (!newTree) {
            if (success) {
                *success = false;
            }
            return fw;
        }

        /* Zero new elements */
        memset(&newTree[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(FENWICK_VALUE_TYPE));

        FENWICK_INDEX_TYPE_FULL oldCount = fw->count;
        fw->tree = newTree;
        fw->count = newCount;
        fw->capacity = newCapacity;

        /* Retroactive propagation */
        if (oldCapacity > 0 && oldCount > 0) {
            for (FENWICK_INDEX_TYPE_FULL pos = 1; pos <= oldCount; pos++) {
                FENWICK_VALUE_TYPE value;
                if (pos == 1) {
                    value = fw->tree[0];
                } else {
                    FENWICK_VALUE_TYPE sum_pos =
                        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
                    FENWICK_VALUE_TYPE sum_prev =
                        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);

                    FENWICK_INDEX_TYPE_FULL p = pos;
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

                FENWICK_INDEX_TYPE_FULL parent = pos + (pos & -pos);
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

    /* BIT update */
    FENWICK_INDEX_TYPE_FULL i = idx + 1;
    while (i <= fw->capacity) {
        fw->tree[i - 1] += delta;
        i = fenwickParent(i);
    }

    if (success) {
        *success = true;
    }
    return fw;
}

/* Query: prefix sum */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX, FullQuery)(
    const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_FULL idx) {
    if (!fw || idx >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE sum =
        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    FENWICK_INDEX_TYPE_FULL i = idx + 1;

    while (i > 0) {
        sum += fw->tree[i - 1];
        i = fenwickPrev(i);
    }

    return sum;
}

/* Range query */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX,
                                                   FullRangeQuery)(
    const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_FULL left, FENWICK_INDEX_TYPE_FULL right) {
    if (!fw || left > right || right >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE rightSum =
        FENWICK_NAME(FENWICK_SUFFIX, FullQuery)(fw, right);

    if (left == 0) {
        return rightSum;
    }

    FENWICK_VALUE_TYPE leftSum =
        FENWICK_NAME(FENWICK_SUFFIX, FullQuery)(fw, left - 1);
    return rightSum - leftSum;
}

/* Get single element */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_NAME(FENWICK_SUFFIX, FullGet)(
    const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
    FENWICK_INDEX_TYPE_FULL idx) {
    if (!fw || idx >= fw->count) {
        return FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);
    }

    FENWICK_VALUE_TYPE current =
        FENWICK_NAME(FENWICK_SUFFIX, FullQuery)(fw, idx);

    if (idx == 0) {
        return current;
    }

    FENWICK_VALUE_TYPE previous =
        FENWICK_NAME(FENWICK_SUFFIX, FullQuery)(fw, idx - 1);
    return current - previous;
}

/* Set single element */
FENWICK_IMPL_SCOPE FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) *
    FENWICK_NAME(FENWICK_SUFFIX,
                 FullSet)(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
                          FENWICK_INDEX_TYPE_FULL idx, FENWICK_VALUE_TYPE value,
                          bool *success) {
    if (!fw) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    FENWICK_VALUE_TYPE current = FENWICK_NAME(FENWICK_SUFFIX, FullGet)(fw, idx);
    FENWICK_VALUE_TYPE delta = value - current;

    return FENWICK_NAME(FENWICK_SUFFIX, FullUpdate)(fw, idx, delta, success);
}

/* Get count */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_FULL
FENWICK_NAME(FENWICK_SUFFIX,
             FullCount)(const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw) {
    return fw ? fw->count : 0;
}

/* Get bytes */
FENWICK_IMPL_SCOPE size_t FENWICK_NAME(FENWICK_SUFFIX, FullBytes)(
    const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        return 0;
    }
    return sizeof(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)) +
           fw->capacity * sizeof(FENWICK_VALUE_TYPE);
}

/* Lower bound */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_FULL
FENWICK_NAME(FENWICK_SUFFIX,
             FullLowerBound)(const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw,
                             FENWICK_VALUE_TYPE target) {
    if (!fw || fw->count == 0) {
        return (FENWICK_INDEX_TYPE_FULL)-1;
    }

    FENWICK_INDEX_TYPE_FULL pos = 0;
    FENWICK_INDEX_TYPE_FULL bitMask = 1;
    while (bitMask <= fw->count) {
        bitMask <<= 1;
    }
    bitMask >>= 1;

    FENWICK_VALUE_TYPE currentSum =
        FENWICK_ZERO(FENWICK_VALUE_TYPE, FENWICK_IS_FLOATING);

    while (bitMask > 0) {
        FENWICK_INDEX_TYPE_FULL nextPos = pos + bitMask;

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

    return (FENWICK_INDEX_TYPE_FULL)-1;
}

/* Clear */
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             FullClear)(FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw || !fw->tree) {
        return;
    }

    memset(fw->tree, 0, fw->capacity * sizeof(FENWICK_VALUE_TYPE));
}

#ifdef DATAKIT_TEST
FENWICK_IMPL_SCOPE void
FENWICK_NAME(FENWICK_SUFFIX,
             FullRepr)(const FENWICK_FULL_TYPENAME(FENWICK_SUFFIX) * fw) {
    if (!fw) {
        printf("FenwickFull: (nil)\n");
        return;
    }

    printf("FenwickFull [count=%lu, capacity=%lu, bytes=%zu]\n",
           (unsigned long)fw->count, (unsigned long)fw->capacity,
           FENWICK_NAME(FENWICK_SUFFIX, FullBytes)(fw));
}
#endif

/* Clean up */
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_INDEX_TYPE_FULL
#undef FENWICK_IS_SIGNED
#undef FENWICK_IS_FLOATING
#undef FENWICK_IMPL_SCOPE
