#pragma once

/* Common utilities for multiFenwick tree system */

#include "databox.h"
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

/* Databox arithmetic operations for Fenwick tree accumulation
 * These handle type-specific addition and subtraction */

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

/* Add two databoxes: result = a + b
 * Supports mixed types with automatic coercion
 * Result type is determined by databoxResultType */
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

    /* For integer-only ops, use integer arithmetic to avoid precision loss */
    if (resType == DATABOX_SIGNED_64 || resType == DATABOX_UNSIGNED_64) {
        /* Extract integer values */
        int64_t va_i = 0, vb_i = 0;
        uint64_t va_u = 0, vb_u = 0;

        if (a->type == DATABOX_SIGNED_64) {
            va_i = a->data.i64;
        } else if (a->type == DATABOX_UNSIGNED_64) {
            va_u = a->data.u64;
        } else if (a->type == DATABOX_VOID) {
            va_i = 0;
        } else {
            goto use_double; /* Fall back to double for mixed float/int */
        }

        if (b->type == DATABOX_SIGNED_64) {
            vb_i = b->data.i64;
        } else if (b->type == DATABOX_UNSIGNED_64) {
            vb_u = b->data.u64;
        } else if (b->type == DATABOX_VOID) {
            vb_i = 0;
        } else {
            goto use_double;
        }

        /* Perform integer addition */
        if (resType == DATABOX_UNSIGNED_64) {
            /* Both are unsigned */
            *result = DATABOX_UNSIGNED(va_u + vb_u);
        } else {
            /* At least one is signed - convert and add */
            int64_t a_signed =
                (a->type == DATABOX_UNSIGNED_64) ? (int64_t)va_u : va_i;
            int64_t b_signed =
                (b->type == DATABOX_UNSIGNED_64) ? (int64_t)vb_u : vb_i;
            *result = DATABOX_SIGNED(a_signed + b_signed);
        }
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
        if (b->type == DATABOX_SIGNED_64) {
            *result = DATABOX_SIGNED(-b->data.i64);
            return true;
        } else if (b->type == DATABOX_UNSIGNED_64) {
            /* Can't negate unsigned properly, promote to signed */
            *result = DATABOX_SIGNED(-(int64_t)b->data.u64);
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

    /* For integer-only ops, use integer arithmetic to avoid precision loss */
    if (resType == DATABOX_SIGNED_64 || resType == DATABOX_UNSIGNED_64) {
        int64_t va_i = 0, vb_i = 0;
        uint64_t va_u = 0, vb_u = 0;

        if (a->type == DATABOX_SIGNED_64) {
            va_i = a->data.i64;
        } else if (a->type == DATABOX_UNSIGNED_64) {
            va_u = a->data.u64;
        } else {
            goto use_double_sub;
        }

        if (b->type == DATABOX_SIGNED_64) {
            vb_i = b->data.i64;
        } else if (b->type == DATABOX_UNSIGNED_64) {
            vb_u = b->data.u64;
        } else {
            goto use_double_sub;
        }

        /* Perform integer subtraction */
        if (resType == DATABOX_UNSIGNED_64) {
            *result = DATABOX_UNSIGNED(va_u - vb_u);
        } else {
            int64_t a_signed =
                (a->type == DATABOX_UNSIGNED_64) ? (int64_t)va_u : va_i;
            int64_t b_signed =
                (b->type == DATABOX_UNSIGNED_64) ? (int64_t)vb_u : vb_i;
            *result = DATABOX_SIGNED(a_signed - b_signed);
        }
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

    /* Mixed types: convert both to double and compare */
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
