#pragma once

/* Common utilities for multiFenwick tree system */

#include "databox.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Inline helper: isolate least significant bit (LSB)
 * This is the core trick of Fenwick trees: x & -x gives 2^r where r is the
 * position of the rightmost 1-bit.
 * Works via two's complement: -x = ~x + 1 = a1b̄ + 1 = ā1b where b are all 0s
 * So x & -x = a1b & ā1b = 1b (only the LSB survives)
 */
static inline uint64_t multiFenwickLSB(uint64_t x) {
    return x & (-(int64_t)x);
}

/* Inline helper: get parent index in BIT (move up tree)
 * Add LSB to current index */
static inline uint64_t multiFenwickParent(uint64_t idx) {
    return idx + multiFenwickLSB(idx);
}

/* Inline helper: get previous index in query (move down tree)
 * Subtract LSB from current index */
static inline uint64_t multiFenwickPrev(uint64_t idx) {
    return idx - multiFenwickLSB(idx);
}

/* Capacity (the smallest power of two, at least 'current', that holds
 * index 'idx') */
static inline uint64_t multiFenwickCapacityFor_(uint64_t current,
                                                uint64_t idx) {
    uint64_t capacity = current == 0 ? 1 : current;
    while (capacity < idx + 1) {
        capacity <<= 1;
    }

    return capacity;
}

/* ====================================================================
 * Arithmetic (shared by multiFenwick and multiFenwickFlat)
 * ====================================================================
 * Every value a tree reads, sums or stores is a multiFenwickNum: an
 * exact integer part 'i' plus a floating part 'd'.  A value with
 * 'isFloat' false is the integer 'i' (and 'd' is 0); otherwise it is
 * i + d, and becomes the double nearest that when it is stored or
 * returned.
 *
 * Integer arithmetic never depends on whether SIGNED_64 or UNSIGNED_64
 * carries a value, and all floating arithmetic runs in double, whatever
 * float width a cell comes back in.
 *
 * A sum over many cells keeps its integer cells exact even when some
 * cells are floats: the integer part only meets the floating part once,
 * at the end. */
typedef struct multiFenwickNum {
    __int128_t i;
    double d;
    bool isFloat;
} multiFenwickNum;

/* The value of 'box', which must be SIGNED_64, UNSIGNED_64, or a finite
 * FLOAT_32 or DOUBLE_64.  Every other type (VOID, BYTES, booleans,
 * pointers, containers, 96/128-bit values) and every NaN or infinity is
 * refused, so no entry point ever stores one. */
static inline bool multiFenwickNumFrom_(const databox *box,
                                        multiFenwickNum *n) {
    switch (box->type) {
    case DATABOX_SIGNED_64:
        *n = (multiFenwickNum){.i = box->data.i64};
        return true;
    case DATABOX_UNSIGNED_64:
        *n = (multiFenwickNum){.i = box->data.u64};
        return true;
    case DATABOX_FLOAT_32:
        if (!isfinite(box->data.f32)) {
            return false;
        }

        *n = (multiFenwickNum){.d = box->data.f32, .isFloat = true};
        return true;
    case DATABOX_DOUBLE_64:
        if (!isfinite(box->data.d64)) {
            return false;
        }

        *n = (multiFenwickNum){.d = box->data.d64, .isFloat = true};
        return true;
    default:
        return false;
    }
}

static inline multiFenwickNum multiFenwickNumAdd_(const multiFenwickNum a,
                                                  const multiFenwickNum b) {
    return (multiFenwickNum){
        .i = a.i + b.i, .d = a.d + b.d, .isFloat = a.isFloat || b.isFloat};
}

static inline multiFenwickNum multiFenwickNumSub_(const multiFenwickNum a,
                                                  const multiFenwickNum b) {
    return (multiFenwickNum){
        .i = a.i - b.i, .d = a.d - b.d, .isFloat = a.isFloat || b.isFloat};
}

/* 'n' as one value: unchanged for an integer, else the double nearest
 * i + d (in 'd', with 'i' zero). */
static inline multiFenwickNum multiFenwickNumCollapse_(multiFenwickNum n) {
    if (n.isFloat && n.i != 0) {
        n.d += (double)n.i;
        n.i = 0;
    }

    return n;
}

/* Box 'n' into 'out'.  Fails, leaving 'out' alone, when 'n' has no 64-bit
 * value: an integer outside [INT64_MIN, UINT64_MAX], or a float that is
 * not finite.  An integer result is SIGNED_64 when negative and
 * UNSIGNED_64 otherwise (the form flex reads it back in); a float result
 * is DOUBLE_64. */
static inline bool multiFenwickNumBox_(multiFenwickNum n, databox *out) {
    n = multiFenwickNumCollapse_(n);
    if (n.isFloat) {
        if (!isfinite(n.d)) {
            return false;
        }

        *out = DATABOX_DOUBLE(n.d);
        return true;
    }

    if (n.i < INT64_MIN || n.i > (__int128_t)UINT64_MAX) {
        return false;
    }

    if (n.i < 0) {
        *out = DATABOX_SIGNED((int64_t)n.i);
    } else {
        *out = DATABOX_UNSIGNED((uint64_t)n.i);
    }

    return true;
}

/* Exact comparison of the integer 'a' with the double 'd' (not NaN).
 * Every integer a tree sums stays far inside +/-2^100. */
static inline int multiFenwickCompareIntDouble_(const __int128_t a,
                                                const double d) {
    if (d >= 0x1p100) {
        return -1;
    }

    if (d <= -0x1p100) {
        return 1;
    }

    /* |d| < 2^100, so its integer part fits, and both the integer part
     * and the fraction left over are exact doubles. */
    const __int128_t whole = (__int128_t)d;
    if (a != whole) {
        return a < whole ? -1 : 1;
    }

    const double frac = d - (double)whole;
    return frac > 0 ? -1 : frac < 0 ? 1 : 0;
}

/* <0, 0 or >0 as a is below, equal to or above b.  Integers compare
 * exactly, with each other and with doubles. */
static inline int multiFenwickNumCompare_(multiFenwickNum a,
                                          multiFenwickNum b) {
    a = multiFenwickNumCollapse_(a);
    b = multiFenwickNumCollapse_(b);
    if (!a.isFloat && !b.isFloat) {
        return (a.i > b.i) - (a.i < b.i);
    }

    if (a.isFloat && b.isFloat) {
        return (a.d > b.d) - (a.d < b.d);
    }

    if (!a.isFloat) {
        return multiFenwickCompareIntDouble_(a.i, b.d);
    }

    return -multiFenwickCompareIntDouble_(b.i, a.d);
}

/* Standalone databox arithmetic helpers.  The trees do not sum with
 * these: they keep exact integer parts and double floating parts (see
 * multiFenwickNum above).  These remain as simple, typed helpers, which
 * the tests also use to build oracles. */

/* Extract numeric value from databox as double (universal representation)
 * Returns true on success */
static inline bool databoxToDouble(const databox *box, double *value) {
    if (!box || !value) {
        return false;
    }

    switch (box->type) {
    case DATABOX_SIGNED_64:
        *value = (double)box->data.i64;
        return true;
    case DATABOX_UNSIGNED_64:
        *value = (double)box->data.u64;
        return true;
    case DATABOX_FLOAT_32:
        *value = (double)box->data.f32;
        return true;
    case DATABOX_DOUBLE_64:
        *value = box->data.d64;
        return true;
    case DATABOX_VOID:
        *value = 0.0;
        return true;
    default:
        return false;
    }
}

/* Create databox from double using specified type */
static inline databox databoxFromDouble(double value, databoxType type) {
    switch (type) {
    case DATABOX_SIGNED_64:
        return DATABOX_SIGNED((int64_t)value);
    case DATABOX_UNSIGNED_64:
        return DATABOX_UNSIGNED((uint64_t)value);
    case DATABOX_FLOAT_32: {
        databox result;
        DATABOX_SET_FLOAT(&result, (float)value);
        return result;
    }
    case DATABOX_DOUBLE_64:
        return DATABOX_DOUBLE(value);
    default:
        return DATABOX_BOX_VOID;
    }
}

/* Determine result type for mixed-type arithmetic
 * Priority: DOUBLE > FLOAT > SIGNED > UNSIGNED */
static inline databoxType databoxResultType(const databox *a,
                                            const databox *b) {
    /* VOID promotes to the other type */
    if (DATABOX_IS_VOID(a)) {
        return b->type;
    }
    if (DATABOX_IS_VOID(b)) {
        return a->type;
    }

    /* If types match, use that type */
    if (a->type == b->type) {
        return a->type;
    }

    /* Mixed types: use higher precision type */
    databoxType ta = a->type;
    databoxType tb = b->type;

    /* DOUBLE_64 has highest priority */
    if (ta == DATABOX_DOUBLE_64 || tb == DATABOX_DOUBLE_64) {
        return DATABOX_DOUBLE_64;
    }

    /* FLOAT_32 next */
    if (ta == DATABOX_FLOAT_32 || tb == DATABOX_FLOAT_32) {
        return DATABOX_FLOAT_32;
    }

    /* Integer ops: preserve UNSIGNED if both are UNSIGNED, else promote to
     * SIGNED */
    if (ta == DATABOX_UNSIGNED_64 && tb == DATABOX_UNSIGNED_64) {
        return DATABOX_UNSIGNED_64;
    }
    if ((ta == DATABOX_SIGNED_64 || ta == DATABOX_UNSIGNED_64) &&
        (tb == DATABOX_SIGNED_64 || tb == DATABOX_UNSIGNED_64)) {
        return DATABOX_SIGNED_64;
    }

    /* Default to first type */
    return ta;
}

/* Exact value of a 64-bit integer box (SIGNED_64 or UNSIGNED_64), or 0 for
 * VOID.  Flex stores integers canonically, so a non-negative value stored
 * as SIGNED_64 reads back as UNSIGNED_64: integer arithmetic must depend
 * on the value, never on which of the two types carries it.
 * Returns false for any other type. */
static inline bool multiFenwickIntValue_(const databox *box, __int128_t *v) {
    switch (box->type) {
    case DATABOX_SIGNED_64:
        *v = box->data.i64;
        return true;
    case DATABOX_UNSIGNED_64:
        *v = box->data.u64;
        return true;
    case DATABOX_VOID:
        *v = 0;
        return true;
    default:
        return false;
    }
}

/* Box the exact integer result 'r'.  A negative result is SIGNED_64 and a
 * result above INT64_MAX is UNSIGNED_64; in between, 'resType' picks the
 * type (both views of the bits agree there).  A result outside the 64-bit
 * range wraps modulo 2^64, as 64-bit integer sums always have here. */
static inline databox multiFenwickIntBox_(__int128_t r, databoxType resType) {
    if (r < 0) {
        return DATABOX_SIGNED((int64_t)(uint64_t)r);
    }

    if (r > INT64_MAX || resType == DATABOX_UNSIGNED_64) {
        return DATABOX_UNSIGNED((uint64_t)r);
    }

    return DATABOX_SIGNED((int64_t)r);
}

/* Add two databoxes: result = a + b
 * Supports mixed types with automatic coercion
 * Result type is determined by databoxResultType, except that an integer
 * result takes the type its value needs (see multiFenwickIntBox_) */
static inline bool databoxAdd(const databox *a, const databox *b,
                              databox *result) {
    if (!a || !b || !result) {
        return false;
    }

    /* Handle VOID cases */
    if (DATABOX_IS_VOID(a) && DATABOX_IS_VOID(b)) {
        *result = DATABOX_BOX_VOID;
        return true;
    }
    if (DATABOX_IS_VOID(a)) {
        *result = *b;
        return true;
    }
    if (DATABOX_IS_VOID(b)) {
        *result = *a;
        return true;
    }

    /* Determine result type */
    databoxType resType = databoxResultType(a, b);

    /* For integer-only ops, use exact integer arithmetic */
    if (resType == DATABOX_SIGNED_64 || resType == DATABOX_UNSIGNED_64) {
        __int128_t va;
        __int128_t vb;
        if (!multiFenwickIntValue_(a, &va) || !multiFenwickIntValue_(b, &vb)) {
            goto use_double; /* not two integers: the double path decides */
        }

        *result = multiFenwickIntBox_(va + vb, resType);
        return true;
    }

use_double:;
    /* Convert both to double for floating point arithmetic */
    double va_d, vb_d;
    if (!databoxToDouble(a, &va_d) || !databoxToDouble(b, &vb_d)) {
        return false;
    }

    *result = databoxFromDouble(va_d + vb_d, resType);
    return true;
}

/* Subtract two databoxes: result = a - b
 * Supports mixed types with automatic coercion */
static inline bool databoxSubtract(const databox *a, const databox *b,
                                   databox *result) {
    if (!a || !b || !result) {
        return false;
    }

    /* Handle VOID cases */
    if (DATABOX_IS_VOID(a) && DATABOX_IS_VOID(b)) {
        *result = DATABOX_BOX_VOID;
        return true;
    }
    if (DATABOX_IS_VOID(a)) {
        /* 0 - b = -b */
        __int128_t vbInt;
        if (multiFenwickIntValue_(b, &vbInt)) {
            *result = multiFenwickIntBox_(-vbInt, DATABOX_SIGNED_64);
            return true;
        }
        double vb;
        if (!databoxToDouble(b, &vb)) {
            return false;
        }
        *result = databoxFromDouble(-vb, b->type);
        return true;
    }
    if (DATABOX_IS_VOID(b)) {
        *result = *a;
        return true;
    }

    /* Determine result type */
    databoxType resType = databoxResultType(a, b);

    /* For integer-only ops, use exact integer arithmetic: two UNSIGNED_64
     * operands (which every non-negative stored value reads back as) give
     * a negative SIGNED_64 when b > a, never a wrapped unsigned value. */
    if (resType == DATABOX_SIGNED_64 || resType == DATABOX_UNSIGNED_64) {
        __int128_t va;
        __int128_t vb;
        if (!multiFenwickIntValue_(a, &va) || !multiFenwickIntValue_(b, &vb)) {
            goto use_double_sub;
        }

        *result = multiFenwickIntBox_(va - vb, resType);
        return true;
    }

use_double_sub:;
    /* Convert both to double for floating point arithmetic */
    double va_d2, vb_d2;
    if (!databoxToDouble(a, &va_d2) || !databoxToDouble(b, &vb_d2)) {
        return false;
    }

    *result = databoxFromDouble(va_d2 - vb_d2, resType);
    return true;
}

/* Compare two databoxes: returns <0 if a<b, 0 if a==b, >0 if a>b
 * Handles mixed types by converting to common representation */
static inline int databoxCompareNumeric(const databox *a, const databox *b) {
    if (!a || !b) {
        return 0;
    }

    /* Handle VOID */
    if (DATABOX_IS_VOID(a) && DATABOX_IS_VOID(b)) {
        return 0;
    }
    if (DATABOX_IS_VOID(a)) {
        return -1;
    }
    if (DATABOX_IS_VOID(b)) {
        return 1;
    }

    /* For same type, compare directly */
    if (a->type == b->type) {
        switch (a->type) {
        case DATABOX_SIGNED_64:
            if (a->data.i64 < b->data.i64) {
                return -1;
            }
            if (a->data.i64 > b->data.i64) {
                return 1;
            }
            return 0;

        case DATABOX_UNSIGNED_64:
            if (a->data.u64 < b->data.u64) {
                return -1;
            }
            if (a->data.u64 > b->data.u64) {
                return 1;
            }
            return 0;

        case DATABOX_FLOAT_32:
            if (a->data.f32 < b->data.f32) {
                return -1;
            }
            if (a->data.f32 > b->data.f32) {
                return 1;
            }
            return 0;

        case DATABOX_DOUBLE_64:
            if (a->data.d64 < b->data.d64) {
                return -1;
            }
            if (a->data.d64 > b->data.d64) {
                return 1;
            }
            return 0;

        default:
            return 0;
        }
    }

    /* Mixed SIGNED_64 / UNSIGNED_64: compare the exact values */
    __int128_t ia;
    __int128_t ib;
    if (multiFenwickIntValue_(a, &ia) && multiFenwickIntValue_(b, &ib)) {
        return ia < ib ? -1 : ia > ib ? 1 : 0;
    }

    /* Other mixed types: convert both to double and compare */
    double va, vb;
    if (!databoxToDouble(a, &va) || !databoxToDouble(b, &vb)) {
        return 0;
    }

    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

/* Create a zero-valued databox of the same type as template */
static inline databox databoxZeroLike(const databox *template) {
    if (!template || DATABOX_IS_VOID(template)) {
        return DATABOX_BOX_VOID;
    }

    switch (template->type) {
    case DATABOX_SIGNED_64:
        return DATABOX_SIGNED(0);
    case DATABOX_UNSIGNED_64:
        return DATABOX_UNSIGNED(0);
    case DATABOX_FLOAT_32: {
        databox result;
        DATABOX_SET_FLOAT(&result, 0.0f);
        return result;
    }
    case DATABOX_DOUBLE_64:
        return DATABOX_DOUBLE(0.0);
    default:
        return DATABOX_BOX_VOID;
    }
}

/* Create a zero-valued databox of a specific type */
static inline databox databoxZeroOfType(databoxType type) {
    switch (type) {
    case DATABOX_SIGNED_64:
        return DATABOX_SIGNED(0);
    case DATABOX_UNSIGNED_64:
        return DATABOX_UNSIGNED(0);
    case DATABOX_FLOAT_32: {
        databox result;
        DATABOX_SET_FLOAT(&result, 0.0f);
        return result;
    }
    case DATABOX_DOUBLE_64:
        return DATABOX_DOUBLE(0.0);
    default:
        return DATABOX_BOX_VOID;
    }
}

/* Convert a databox to a target type (best effort numeric conversion)
 * Returns converted databox, or VOID if conversion not possible */
static inline databox databoxConvertToType(const databox *src,
                                           databoxType targetType) {
    if (!src || targetType == DATABOX_VOID) {
        return DATABOX_BOX_VOID;
    }

    /* If already correct type, return as-is */
    if (src->type == targetType) {
        return *src;
    }

    /* Extract numeric value from source */
    double value = 0.0;
    switch (src->type) {
    case DATABOX_SIGNED_64:
        value = (double)src->data.i64;
        break;
    case DATABOX_UNSIGNED_64:
        value = (double)src->data.u64;
        break;
    case DATABOX_FLOAT_32:
        value = (double)src->data.f32;
        break;
    case DATABOX_DOUBLE_64:
        value = src->data.d64;
        break;
    case DATABOX_VOID:
        value = 0.0;
        break;
    default:
        return DATABOX_BOX_VOID;
    }

    /* Convert to target type */
    switch (targetType) {
    case DATABOX_SIGNED_64:
        return DATABOX_SIGNED((int64_t)value);
    case DATABOX_UNSIGNED_64:
        return DATABOX_UNSIGNED((uint64_t)value);
    case DATABOX_FLOAT_32: {
        databox result;
        DATABOX_SET_FLOAT(&result, (float)value);
        return result;
    }
    case DATABOX_DOUBLE_64:
        return DATABOX_DOUBLE(value);
    default:
        return DATABOX_BOX_VOID;
    }
}
