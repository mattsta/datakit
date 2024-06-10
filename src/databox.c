#include "databox.h"
#include "str.h"

#if DATABOX_ENABLE_PTR_MDSC
#include "mdsc.h"
#endif

const databox DATABOX_BOX_TRUE = {.type = DATABOX_TRUE};
const databox DATABOX_BOX_FALSE = {.type = DATABOX_FALSE};
const databox DATABOX_BOX_NULL = {.type = DATABOX_NULL};
const databox DATABOX_BOX_VOID = {.type = DATABOX_VOID};

/* 'box' isn't 'const' here because if type is EMBED, we're
 * returning data _inside_ the box itself, not pointed to by 'box' */
bool databoxGetBytes(databox *box, uint8_t **buf, size_t *len) {
    if (!buf || !len) {
        return false;
    }

    switch (box->type) {
    case DATABOX_BYTES_EMBED:
        *buf = box->data.bytes.embed;
        *len = box->len;
        break;
    case DATABOX_BYTES:
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
    case DATABOX_CONTAINER_CFLEX_MAP:
    case DATABOX_CONTAINER_CFLEX_LIST:
    case DATABOX_CONTAINER_CFLEX_SET:
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        *buf = box->data.bytes.start;
        *len = box->len;
        break;
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
    case DATABOX_DOUBLE_64:
        *buf = (uint8_t *)&box->data.i;
        *len = 8;
        break;
    case DATABOX_FLOAT_32:
        *buf = (uint8_t *)&box->data.f32;
        *len = 4;
        break;
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
        return NULL; /* not implemented yet! */
    default:
        assert(NULL && "Unsupported bytes type?");
        return false;
    }

    return true;
}

bool databoxGetSize(const databox *box, size_t *len) {
    if (!box || !len) {
        return false;
    }

    switch (box->type) {
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
    case DATABOX_CONTAINER_CFLEX_MAP:
    case DATABOX_CONTAINER_CFLEX_LIST:
    case DATABOX_CONTAINER_CFLEX_SET:
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        *len = box->len;
        break;
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
    case DATABOX_DOUBLE_64:
        *len = 8;
        break;
    case DATABOX_FLOAT_32:
        *len = 4;
        break;
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
        *len = 0;
        break;
    default:
        return false;
    }

    return true;
}

size_t databoxGetSizeMinimum(const databox *box) {
    switch (box->type) {
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
    case DATABOX_CONTAINER_CFLEX_MAP:
    case DATABOX_CONTAINER_CFLEX_LIST:
    case DATABOX_CONTAINER_CFLEX_SET:
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        return box->len;
        break;
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
    case DATABOX_DOUBLE_64:
        return 8;
        break;
    case DATABOX_FLOAT_32:
        return 4;
        break;
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
        return 0;
        break;
    default:
        assert(NULL && "Invalid type!");
        return 0;
    }

    assert(NULL && "Really Wrong!");
    __builtin_unreachable();
}

databox databoxVoid(void) {
    return DATABOX_BOX_VOID;
}

void databoxCopyBytes(databox *box, const void *ptr, size_t len) {
    box->type = DATABOX_BYTES;
    box->allocated = true;
    box->len = len;
    box->data.bytes.start = zmalloc(len);
    memcpy(box->data.bytes.start, ptr, len);
}

/* A more pre-prepared version of databoxRetainBytesSelf() where the
 * memory size determination has been done out of band of databox.
 * This version also pre-supposes you're only passing in type DATABOX_BYTES */
void databoxRetainBytesSelfExact(databox *restrict dst, void *restrict bytes) {
    if (dst->len <= sizeof(dst->data.bytes.embed8)) {
        /* If source bytes small enough to become embed, do
         * embed instead of allocating actual memory */
        memcpy(dst->data.bytes.embed8, dst->data.bytes.start, databoxLen(dst));
        dst->type = DATABOX_BYTES_EMBED;

        /* Success! */
        return;
    }

    /* do the copy do do do da do do do — do the copy */
    dst->allocated = true;
    memcpy(bytes, dst->data.bytes.start, dst->len);

    dst->data.bytes.start = bytes;
}

/* Uses bytes provided in 'cache' to copy bytes from 'dst' into a private
 * memory space if necessary.
 *
 * Returns the slot used in 'cache' if a slot is used or a negative number
 * if no slot is used. */
ssize_t databoxRetainBytesSelf(databox *restrict dst,
                               databoxRetainCache *restrict cache) {
    /* NOTE: this breaks with offset boxes */
    if (dst->type >= DATABOX_BYTES) {
        if (dst->len <= sizeof(dst->data.bytes.embed8)) {
            /* If source bytes small enough to become embed, do
             * embed instead of allocating actual memory */
            memcpy(dst->data.bytes.embed8, dst->data.bytes.start,
                   databoxLen(dst));
            dst->type = DATABOX_BYTES_EMBED;

            /* Success, but didn't use any 'cache' bytes */
            return -1;
        }

        const uint8_t *restrict src = dst->data.bytes.start;
        size_t start = 128;
        ssize_t slot = 0;
        /* While bytes are too big for current slot
         * AND
         * slot is less than total slots
         * AND
         * slot is actually populated;
         * increment slot position and check next power of two cache size */
        for (; slot < (ssize_t)COUNT_ARRAY(cache->bytes); slot++, start *= 2) {
            if (dst->len <= start) {
                /* Point bytes to new memory to use for copy */
                if (cache->bytes[slot]) {
                    /* use cached bytes! */
                    dst->data.bytes.start = cache->bytes[slot];
                } else {
                    /* too big for any cached containers */
                    dst->data.bytes.start = zmalloc(dst->len);
                    slot = -1; /* return value; no slot used */
                }

                /* do tha copy do do do da do do do — do tha copy */
                dst->allocated = true;
                memcpy(dst->data.bytes.start, src, dst->len);
                return slot;
            }
        }
    }

    return -2;
}

void databoxCopyBytesFromBox(databox *dst, const databox *src) {
    *dst = *src;
    if (DATABOX_IS_BYTES_EMBED(src)) {
        /* Nothing remaining to copy since bytes are embeded */
        return;
    }

    /* NOTE: this breaks with offset boxes */
    if (src->type >= DATABOX_BYTES) {
        if (src->len <= sizeof(dst->data.bytes.embed8)) {
            /* If source bytes small enough to become embed, do
             * embed instead of allocating actual memory */
            memcpy(dst->data.bytes.embed8, databoxBytes(src), databoxLen(src));
            dst->type = DATABOX_BYTES_EMBED;
        } else {
            const void *srcBytes = src->data.bytes.start;
            dst->allocated = true;
            dst->data.bytes.start = zcalloc(1, src->len);
            memcpy(dst->data.bytes.start, srcBytes, src->len);
        }
    }
}

bool databoxAllocateIfNeeded(databox *restrict box) {
    if (DATABOX_IS_BYTES_EMBED(box)) {
        /* Nothing remaining to copy since bytes are embeded */
        return false;
    }

    if (box->type >= DATABOX_BYTES && box->type < DATABOX_MAX_EMBED) {
        const void *restrict srcBytes = box->data.bytes.start;
        box->allocated = true;
        box->data.bytes.start = zmalloc(box->len);
        memcpy(box->data.bytes.start, srcBytes, box->len);
        return true;
    }

    return false;
}

databox databoxCopy(const databox *restrict src) {
    databox dst_ = *src;
    databox *restrict dst = &dst_;
    if (DATABOX_IS_BYTES_EMBED(src)) {
        /* Nothing remaining to copy since bytes are embeded */
        return dst_;
    }

    /* NOTE: this breaks with offset boxes */
    if (src->type >= DATABOX_BYTES) {
        const void *restrict srcBytes = src->data.bytes.start;
        dst->allocated = true;
        dst->data.bytes.start = zcalloc(1, src->len);
        memcpy(dst->data.bytes.start, srcBytes, src->len);
    }

    return dst_;
}

void databoxFreeData(databox *box) {
    if (box) {
        if (box->allocated) {
            if (box->type == DATABOX_BYTES) {
                /*  box->type != DATABOX_BYTES_NEVER_FREE && box->allocated) {
                 */
                zfree(box->data.ptr);
                box->data.ptr = NULL;
                box->allocated = false;
                box->type = DATABOX_VOID;
            } else {
#if DATABOX_ENABLE_PTR_MDSC
                if (box->type == DATABOX_PTR_MDSC) {
                    mdscfree(box->data.ptr);
                    box->data.ptr = NULL;
                    box->allocated = false;
                    box->type = DATABOX_VOID;
                }
#endif
                /* Other types? */
            }
        }
    }
}

void databoxFree(databox *box) {
    if (box) {
        databoxFreeData(box);
        zfree(box);
    }
}

databox databoxNewBytes(const void *ptr, size_t len) {
    databox box;
    box.type = DATABOX_BYTES;
    box.data.bytes.custart = ptr;
    box.len = len;
    return box;
}

databox databoxNewBytesAllowEmbed(const void *ptr, size_t len) {
    databox box;
    databoxUpdateBytesAllowEmbed(&box, ptr, len);
    return box;
}

databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len) {
    databox box;
    if (len <= sizeof(box.data.bytes.embed)) {
        box.type = DATABOX_BYTES_EMBED;
        box.len = len;
        memcpy(box.data.bytes.embed, ptr, len);
    } else {
        databoxCopyBytes(&box, ptr, len);
    }

    return box;
}

databox databoxNewOffsetAllowEmbed(const void *ptr, size_t offset, size_t len) {
    databox box;
    databoxUpdateOffsetAllowEmbed(&box, ptr, offset, len);
    return box;
}

databox databoxNewBytesString(const char *str) {
    return databoxNewBytes(str, strlen(str));
}

databox databoxNewUnsigned(uint64_t val) {
    databox box = {.type = DATABOX_UNSIGNED_64, .data.u = val};
    return box;
}

databox databoxNewSigned(int64_t val) {
    databox box = {.type = DATABOX_SIGNED_64, .data.i = val};
    return box;
}

databox databoxNewReal(double value) {
    databox box = {{0}};

    if ((double)(float)value == value) {
        /* double fits in float! */
        box.type = DATABOX_FLOAT_32;
        box.data.f32 = value;
    } else {
        box.type = DATABOX_DOUBLE_64;
        box.data.d64 = value;
    }

    return box;
}

databox databoxNewPtr(void *ptr) {
    databox box = {.type = DATABOX_PTR, .data.ptr = ptr};
    return box;
}

databox databoxBool(bool which) {
    return which ? DATABOX_BOX_TRUE : DATABOX_BOX_FALSE;
}

databox databoxNull(void) {
    return DATABOX_BOX_NULL;
}

#define IS_BYTES(a) ((a)->type >= DATABOX_BYTES)
#define IS_INTEGER(a)                                                          \
    (DATABOX_IS_INTEGER(a) || (a)->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL)

bool databoxEqual(const databox *a, const databox *b) {
    return databoxCompare(a, b) == 0;
}

#if 1
#define _gtlteqReturn(a, b) return (((a) < (b)) ? -1 : ((a) > (b)))
#else
#define _gtlteqReturn(a, b)                                                    \
    do {                                                                       \
        if ((a) > (b)) {                                                       \
            return +1;                                                         \
        }                                                                      \
                                                                               \
        if ((a) < (b)) {                                                       \
            return -1;                                                         \
        }                                                                      \
                                                                               \
        return 0;                                                              \
    } while (0)
#endif

DK_INLINE_ALWAYS int databoxCompareBytes_(const uint8_t *aBytes,
                                          const size_t aLen,
                                          const uint8_t *bBytes,
                                          const size_t bLen) {
    int_fast32_t biggerIntegerRepresentation = 0;
    const bool longestIsA = aLen > bLen;
    const size_t maxLen = longestIsA ? bLen : aLen;
    for (size_t i = 0; i < maxLen; i++) {
        uint_fast8_t aa = aBytes[i];
        uint_fast8_t bb = bBytes[i];
        if (StrIsdigit(aa) && StrIsdigit(bb)) {
            if (biggerIntegerRepresentation == 0) {
                /* If everything has been a number so far, only track
                 * the first different integer as we go along */
                if (aa < bb) {
                    biggerIntegerRepresentation = -1;
                } else if (aa > bb) {
                    biggerIntegerRepresentation = 1;
                }
            }

            continue;
        }

        if (biggerIntegerRepresentation) {
            if (!StrIsdigit(aa) && !StrIsdigit(bb)) {
                goto stringIsNotEntirelyNumeric;
            }

            /* We were numeric up to this point, but... */

            /* a is no longer numeric, so return a < b */
            if (!StrIsdigit(aa)) {
                return -1;
            }

            /* b is no longer numeric, so return a > b */
            if (!StrIsdigit(bb)) {
                return 1;
            }
        }

    stringIsNotEntirelyNumeric:
        if (aa < bb) {
            return -1;
        }

        if (aa > bb) {
            return 1;
        }

        /* else, bytes are equal and we need to check the next pair of bytes */
    }

    /* If lengths are equal and all bytes have been numbers... */
    if (aLen == bLen && biggerIntegerRepresentation) {
        return biggerIntegerRepresentation;
    }

    /* As of here, strings are equal as of the smallest length,
     * so if A is bigger than B, return positive. */
    if (longestIsA) {
        return 1;
    }

    if (aLen == bLen) {
        return 0;
    }

    /* else, longestIsB */
    return -1;
}

DK_INLINE_ALWAYS int databoxCompareBytes(const databox *a, const databox *b) {
    return databoxCompareBytes_(databoxBytes(a), a->len, databoxBytes(b),
                                b->len);
}

#define c1b(a, b) ((a) << 1 | (b))
#define c3b(a, b) ((a) << 3 | (b))
#define c3bMAX_LT (1 << 3)
DK_INLINE_ALWAYS int databoxCompareInteger(const databox *a, const databox *b) {
    const size_t aSigned = a->type == DATABOX_SIGNED_64;
    const size_t bSigned = b->type == DATABOX_SIGNED_64;

#if 1
    switch (c1b(aSigned, bSigned)) {
    case c1b(false, false):
        /* Simplest case: two unsigned values, compare as unsigned. */
        _gtlteqReturn(a->data.u, b->data.u);
    case c1b(false, true):
        /* If b is negative, then a is larger, OR
         * If a's unsigned value is larger than the largest
         * possible signed value, it's clearly bigger too. */
        if (b->data.i < 0 || a->data.u > INTMAX_MAX) {
            return 1;
        }

        /* else, we can safely cast a->data.u to a signed int
         * without losing any precision. */
        _gtlteqReturn(a->data.i, b->data.i);
    case c1b(true, false):
        if (a->data.i < 0 || b->data.u > INTMAX_MAX) {
            return -1;
        }

        /* fallthrough */
    case c1b(true, true):
        /* Here both values are signed (or we've restricted
         * their values to be compared as signed), so compare
         * as signed: */
        _gtlteqReturn(a->data.i, b->data.i);
    default:
        __builtin_unreachable();
    }
#else
    if (!aSigned && !bSigned) {
        _gtlteqReturn(a->data.u, b->data.u);
    }

    /* B < A */
    if (!aSigned && bSigned && b->data.i < 0) {
        /* If comparing unsigned and signed, and signed is negative,
         * the signed value is lower by default. */
        return +1;
    }

    /* A < B */
    if (aSigned && !bSigned && a->data.i < 0) {
        return -1;
    }

    /* A ?? B */
    _gtlteqReturn(a->data.i, b->data.i);
#endif
}

DK_INLINE_ALWAYS int databoxCompareInteger128(const databox *a,
                                              const databox *b) {
    const size_t aSigned =
        (a->type == DATABOX_SIGNED_64) || (a->type == DATABOX_SIGNED_128);
    const size_t bSigned =
        (b->type == DATABOX_SIGNED_64) || (b->type == DATABOX_SIGNED_128);

    switch (c1b(aSigned, bSigned)) {
    case c1b(false, false): {
        __uint128_t ua =
            a->type == DATABOX_UNSIGNED_64 ? a->data.u : *a->data.u128;
        __uint128_t ub =
            b->type == DATABOX_UNSIGNED_64 ? b->data.u : *b->data.u128;

        /* Simplest case: two unsigned values, compare as unsigned. */
        _gtlteqReturn(ua, ub);
    }
    case c1b(false, true): {
        __uint128_t ua =
            a->type == DATABOX_UNSIGNED_64 ? a->data.u : *a->data.u128;
        __int128_t ib =
            b->type == DATABOX_SIGNED_64 ? b->data.i : *b->data.i128;

        /* If b is negative, then a is larger, OR
         * If a's unsigned value is larger than the largest
         * possible signed value, it's clearly bigger too. */
        if (ib < 0 || ua > INT128_MAX) {
            return 1;
        }

        /* else, we can safely cast a->data.u to a signed int
         * without losing any precision. */
        _gtlteqReturn(ua, (__uint128_t)ib);
    }
    case c1b(true, false): {
        __int128_t ia =
            a->type == DATABOX_SIGNED_64 ? a->data.i : *a->data.i128;
        __uint128_t ub =
            b->type == DATABOX_UNSIGNED_64 ? b->data.u : *b->data.u128;

        if (ia < 0 || ub > INT128_MAX) {
            return -1;
        }

        _gtlteqReturn(a->data.i, b->data.i);
    }
    case c1b(true, true): {
        /* Here both values are signed (or we've restricted
         * their values to be compared as signed), so compare
         * as signed: */
        __int128_t ia =
            a->type == DATABOX_SIGNED_64 ? a->data.i : *a->data.i128;
        __int128_t ib =
            b->type == DATABOX_SIGNED_64 ? b->data.i : *b->data.i128;
        _gtlteqReturn(ia, ib);
    }
    default:
        __builtin_unreachable();
    }
}

DK_INLINE_ALWAYS int databoxCompareFloat(const databox *a, const databox *b) {
    const size_t aFloat = a->type == DATABOX_FLOAT_32;
    const size_t bFloat = b->type == DATABOX_FLOAT_32;

#if 1
    const double useA = aFloat ? a->data.f32 : a->data.d64;
    const double useB = bFloat ? b->data.f32 : b->data.d64;

    _gtlteqReturn(useA, useB);
#else
    useA = a->data.d64;
    useB = b->data.d64;

    if (aFloat && bFloat) {
        useA = a->data.f32;
        useB = b->data.f32;
    } else if (aFloat && !bFloat) {
        useA = a->data.f32;
    } else if (!aFloat && bFloat) {
        useB = b->data.f32;
    }

    _gtlteqReturn(useA, useB);
#endif
}

/*
** Do a comparison between a 64-bit signed integer and a 64-bit floating-point
** number.  Return negative, zero, or positive if the first (int64_t) is less
** than, equal to, or greater than the second (double).
*/
/* Logic initially from sqlite3; adapted to databox+unsigned+float */
DK_INLINE_ALWAYS int databoxCompareInt64Float(const databox *a,
                                              const databox *b) {
    assert(IS_INTEGER(a) && DATABOX_IS_FLOAT(b));

    const bool aIsUnsigned = a->type == DATABOX_UNSIGNED_64;
    const double r =
        b->type == DATABOX_FLOAT_32 ? (double)b->data.f32 : b->data.d64;

    /* If we have usable long double type, just compare directly. */
    if (sizeof(long double) > 8) {
        const long double x =
            aIsUnsigned ? (long double)a->data.u : (long double)a->data.i;
        _gtlteqReturn(x, r);
    }

    /* On modern platfoms we never reach here because at compile time the
     * previous if() is true, so the rest of this code is entire ommitted
     * from builds. */

    /* These are fallback cases if we end up on a platform without
     * 80+ bit long doubles, but why would we be on those platforms? */
    if (r < -9223372036854775808.0) {
        return +1;
    }

    if (r > 9223372036854775807.0) {
        return -1;
    }

    const int64_t i = a->data.i;
    const uint64_t u = a->data.u;
    if (aIsUnsigned) {
        /* If a is unsigned but b is negative, b is less than a. */
        if (r < 0) {
            return -1;
        }

        const uint64_t y = (uint64_t)r;
        if (u < y) {
            return -1;
        }

        if (u > y) {
            if ((int64_t)y == INT64_MIN && r > 0.0) {
                return -1;
            }

            return +1;
        }

        const double s = (double)i;
        _gtlteqReturn(s, r);
    } else {
        const int64_t y = (int64_t)r;
        if (i < y) {
            return -1;
        }

        if (i > y) {
            if (y == INT64_MIN && r > 0.0) {
                return -1;
            }

            return +1;
        }

        const double s = (double)i;
        _gtlteqReturn(s, r);
    }
}

DK_INLINE_ALWAYS int databoxCompareInt128Float(const databox *a,
                                               const databox *b) {
    assert(IS_INTEGER(a) && DATABOX_IS_FLOAT(b));

    const bool aIsUnsigned =
        (a->type == DATABOX_UNSIGNED_64) || (a->type == DATABOX_UNSIGNED_128);
    const double r =
        b->type == DATABOX_FLOAT_32 ? (double)b->data.f32 : b->data.d64;

    /* If we have usable long double type, just compare directly. */
#if __x86_64__
    _Static_assert(sizeof(long double) > 8,
                   "Your long double isn't long enough and we don't have a "
                   "fallback here!");
#else
#warning                                                                       \
    "Using unoptimized float compare because system doesn't have a valid long double."
#endif

    if (sizeof(long double) > 8) {
        /* Convert 64-bit integers to > 8 byte floats for direct compares
         * without needing any bounds checking. */
        if (aIsUnsigned) {
            const __uint128_t ua =
                (a->type == DATABOX_UNSIGNED_64) ? a->data.u : *a->data.u128;
            const long double x = (long double)ua;
            _gtlteqReturn(x, r);
        }

        const __int128_t ia =
            (a->type == DATABOX_SIGNED_64) ? a->data.i : *a->data.i128;
        const long double x = (long double)ia;
        _gtlteqReturn(x, r);

    } else {
        /* else, do the opposite and convert the float to __int128 (loses float
         * precision) then smash that like button. this may cause loss of
         * precision, but does it? somebody should test outcomes here. */
        /* TODO: should probably do better range checking here like:
         *          - if r > UINT128_MAX, r is bigger
         *          - if r < 0 and unsigned, r is smaller
         *          - if r > INT128_MAX, r is bigeger if signed
         *          - if r < INT128_MIN, r is smaller, etc
         *          - else we know r is in the INT128 range so we can cast and
         * compare.
         *          - note though: can give false order if like BIGNUMBER.333 is
         * float and BIGNUMBER is float128 but the conversion just kills the
         * .333, so maybe a ceiling conversion for order compare? */
        /* TODO: these need mondo tests and perhaps better type constraints...
         */
        if (aIsUnsigned) {
            if (r < 0) {
                /* A > B */
                return 1;
            }

            if (r > (double)UINT128_MAX) {
                /* A < B */
                return -1;
            }

            const __uint128_t ua =
                (a->type == DATABOX_UNSIGNED_64) ? a->data.u : *a->data.u128;

            /* Now we know 'r' can fit in the UINT128 range... */
            /* We don't necessarily care about exact accuracy here as
             * long as the result is just consistent for same-ordering of
             * same-data each time. Only risk here is if two items compare equal
             * when they are not equal so their orders are not stable in a
             * discovery process. */
            const __uint128_t r_t = (__uint128_t)r;
            _gtlteqReturn(ua, r_t);
        }

        if (r < (double)INT128_MIN) {
            /* A > B */
            return 1;
        }

        if (r > (double)INT128_MAX) {
            /* A < B */
            return -1;
        }

        const __int128_t ia =
            (a->type == DATABOX_SIGNED_64) ? a->data.i : *a->data.i128;

        const __int128_t r_t = (__int128_t)r;
        _gtlteqReturn(ia, r_t);
    }
}

int databoxCompare(const databox *restrict a, const databox *restrict b) {
#if 1
    if (IS_BYTES(a) && IS_BYTES(b)) {
        return databoxCompareBytes(a, b);
    }

    /* Note: we use the 3 bit shift here instead of aligning the
     *       types to byte boundaries (i.e. c8b) because compilers
     *       generate more optimal code this way.
     *       When using c8b(), compilers generated 'cmp' instructions
     *       for each case. Using c3b(), compilers generate jump tables.
     *
     *       This works because DATABOX_SIGNED_64 through DATABOX_DOUBLE_64
     *       are sequential in the range 2, 3, 4, 5 (i.e. maximum 3 bits).
     *
     *       Also, DATABOX_SIGNED_128 and DATABOX_UNSIGNED_128 are in positions
     *       6 and 7 which still fill out the maximum 3 bits */

    /* Verify types of 'a' and 'b' are no larger than 3 bits each. */
    if (a->type < c3bMAX_LT && b->type < c3bMAX_LT) {
        assert(a->type < (1 << 3));
        assert(b->type < (1 << 3));
        switch (c3b(a->type, b->type)) {
        /* Common cases */
        case c3b(DATABOX_UNSIGNED_64, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_UNSIGNED_64, DATABOX_SIGNED_64):
        case c3b(DATABOX_SIGNED_64, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_SIGNED_64, DATABOX_SIGNED_64):
            return databoxCompareInteger(a, b);
        case c3b(DATABOX_FLOAT_32, DATABOX_FLOAT_32):
        case c3b(DATABOX_FLOAT_32, DATABOX_DOUBLE_64):
        case c3b(DATABOX_DOUBLE_64, DATABOX_FLOAT_32):
        case c3b(DATABOX_DOUBLE_64, DATABOX_DOUBLE_64):
            return databoxCompareFloat(a, b);
        case c3b(DATABOX_UNSIGNED_64, DATABOX_FLOAT_32):
        case c3b(DATABOX_UNSIGNED_64, DATABOX_DOUBLE_64):
        case c3b(DATABOX_SIGNED_64, DATABOX_FLOAT_32):
        case c3b(DATABOX_SIGNED_64, DATABOX_DOUBLE_64):
            return databoxCompareInt64Float(a, b);
        case c3b(DATABOX_FLOAT_32, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_FLOAT_32, DATABOX_SIGNED_64):
        case c3b(DATABOX_DOUBLE_64, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_DOUBLE_64, DATABOX_SIGNED_64):
            return -databoxCompareInt64Float(b, a);

        /* Bigger cases */
        case c3b(DATABOX_UNSIGNED_128, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_UNSIGNED_128, DATABOX_SIGNED_128):
        case c3b(DATABOX_UNSIGNED_128, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_UNSIGNED_128, DATABOX_SIGNED_64):
        case c3b(DATABOX_SIGNED_128, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_SIGNED_128, DATABOX_SIGNED_128):
        case c3b(DATABOX_SIGNED_128, DATABOX_UNSIGNED_64):
        case c3b(DATABOX_SIGNED_128, DATABOX_SIGNED_64):
        case c3b(DATABOX_UNSIGNED_64, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_UNSIGNED_64, DATABOX_SIGNED_128):
        case c3b(DATABOX_SIGNED_64, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_SIGNED_64, DATABOX_SIGNED_128):
            return databoxCompareInteger128(a, b);
        case c3b(DATABOX_UNSIGNED_128, DATABOX_FLOAT_32):
        case c3b(DATABOX_UNSIGNED_128, DATABOX_DOUBLE_64):
        case c3b(DATABOX_SIGNED_128, DATABOX_FLOAT_32):
        case c3b(DATABOX_SIGNED_128, DATABOX_DOUBLE_64):
            return databoxCompareInt128Float(a, b);
        case c3b(DATABOX_FLOAT_32, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_FLOAT_32, DATABOX_SIGNED_128):
        case c3b(DATABOX_DOUBLE_64, DATABOX_UNSIGNED_128):
        case c3b(DATABOX_DOUBLE_64, DATABOX_SIGNED_128):
            return -databoxCompareInt128Float(b, a);
        default:
#ifndef NDEBUG
            printf("Attempted to compare: %d and %d?", a->type, b->type);
#endif
            assert(NULL && "Either you tried to compare against DATABOX_VOID "
                           "or we created new numeric types and didn't add "
                           "corresponding comparison table entries.");
            __builtin_unreachable();
        }
    } else {
        /* This looks weird since we have that big jump table for
         * numeric type compares above, but IS_INTEGER
         * checks for (IS_SIGNED, IS_UNSIGNED, IS_REFERENCE) while the
         * above switch only checks for (IS_SIGNED, IS_UNSIGNED).
         *
         * We don't want to pollute the above jump table with
         * 2^3 integer cases per function when 2^2 signed/unsigned combinations
         * are more likely.
         * So, if we fell through to here, check one more time: */
        if (IS_INTEGER(a) && IS_INTEGER(b)) {
            return databoxCompareInteger(a, b);
        }

        /* If types are the same by this point, we
         * have immediate types (true, false, null) and they
         * compare equal.
         * Yes, we let NULL compare equal to NULL. */
        if (a->type == b->type) {
            return 0;
        }

        /* else, sort based on thier incompatible type ordering */
        _gtlteqReturn(a->type, b->type);
    }
#else
    /* Compare bytes against bytes */
    if (IS_BYTES(a) && IS_BYTES(b)) {
        /* Right now all bytes are stringy/blobs, but we don't
         * collate any sequences.  We *do* attempt to sort
         * based on human-readable integers inside of strings though. */
        return databoxCompareBytes(a, b);
    }

    /* Compare {signed,unsigned} against {signed,unsigned} */
    if (IS_INTEGER(a) && IS_INTEGER(b)) {
        return databoxCompareInteger(a, b);
    }

    /* Compare {float,double} against {float,double} */
    if (DATABOX_IS_FLOAT(a) && DATABOX_IS_FLOAT(b)) {
        return databoxCompareFloat(a, b);
    }

    /* Compare {signed,unsigned} against {float,double} */
    if (IS_INTEGER(a) && DATABOX_IS_FLOAT(b)) {
        return databoxCompareInt64Float(a, b);
    }

    /* Compare {float,double} against {signed,unsigned} */
    if (DATABOX_IS_FLOAT(a) && IS_INTEGER(b)) {
        /* We flip the return value because notice we flip
         * the argument order because the function only takes
         * args in order: INTEGER, FLOAT */
        return -databoxCompareInt64Float(b, a);
    }

    /* Compare types directly */
    if (a->type == b->type) {
        /* else, we have same immediate types
         * (i.e. TRUE == TRUE, FALSE == FALSE, NULL == NULL;
         *  fixed values with no user data) */
        /* We allow NULL to equal NULL because this isn't SQL */
        return 0;
    }

    /* Types don't match and aren't directly convertable, so
     * compare using a cross-type sort order of:
     *   void < bytes < integers < floats < true < false < null */

    /* Just sort non-comparable types by their enum position designation */
    _gtlteqReturn(a->type, b->type);
#endif
}

#ifdef DATAKIT_TEST
#include <stdio.h> /* printf */
const char *databoxReprStr(const databox *const box) {
    _Thread_local static char buf[256];
    _Thread_local static char bufBig[COUNT_ARRAY(buf)];
    static const size_t bufLen = COUNT_ARRAY(buf);

    switch (box->type) {
    case DATABOX_VOID:
        snprintf(buf, bufLen, "{VOID}");
        break;
    case DATABOX_ERROR:
        snprintf(buf, bufLen, "{ERROR}");
        break;
    case DATABOX_SIGNED_64:
        snprintf(buf, bufLen, "{SIGNED: %" PRId64 "}", box->data.i64);
        break;
    case DATABOX_UNSIGNED_64:
        snprintf(buf, bufLen, "{UNSIGNED: %" PRIu64 "}", box->data.u64);
        break;
    case DATABOX_SIGNED_128: {
        assert(box->big);
        const int written =
            StrInt128ToBuf(bufBig, sizeof(bufBig), *box->data.i128);
        snprintf(buf, bufLen, "{SIGNED128: %.*s}", written, bufBig);
        break;
    }
    case DATABOX_UNSIGNED_128: {
        assert(box->big);
        const int written =
            StrUInt128ToBuf(bufBig, sizeof(bufBig), *box->data.u128);
        snprintf(buf, bufLen, "{UNSIGNED128: %.*s", written, bufBig);
        break;
    }
    case DATABOX_FLOAT_32:
        snprintf(buf, bufLen, "{FLOAT: %f}", box->data.f32);
        break;
    case DATABOX_DOUBLE_64:
        snprintf(buf, bufLen, "{DOUBLE: %f}", box->data.d64);
        break;
    case DATABOX_TRUE:
        snprintf(buf, bufLen, "{TRUE}");
        break;
    case DATABOX_FALSE:
        snprintf(buf, bufLen, "{FALSE}");
        break;
    case DATABOX_NULL:
        snprintf(buf, bufLen, "{NULL}");
        break;
    case DATABOX_PTR:
        snprintf(buf, bufLen, "{PTR: %p}", box->data.ptr);
        break;
#if DATABOX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
        snprintf(buf, bufLen, "{PTR MDSC: %p (%s)}", box->data.ptr,
                 box->data.bytes.start);
        break;
#endif
    case DATABOX_CONTAINER_REFERENCE_EXTERNAL: /* is unsigned */
        snprintf(buf, bufLen, "{EXTERNAL REF: %" PRIu64 "}", box->data.u64);
        break;
    case DATABOX_BYTES:
        snprintf(buf, bufLen, "{BYTES: %.*s}", (int)box->len,
                 box->data.bytes.start);
        break;
    case DATABOX_BYTES_NEVER_FREE:
        snprintf(buf, bufLen, "{BYTES (NEVER FREE): %.*s}", (int)box->len,
                 box->data.bytes.start);
        break;
    case DATABOX_BYTES_OFFSET:
        snprintf(buf, bufLen, "{BYTES OFFSET START AT: %zu}",
                 box->data.bytes.offset);
        break;
    case DATABOX_BYTES_EMBED:
        snprintf(buf, bufLen, "{BYTES EMBED: %.*s}", (int)box->len,
                 box->data.bytes.cembed);
        break;
    case DATABOX_CONTAINER_FLEX_MAP:
        snprintf(buf, bufLen, "{FLEX MAP}");
        break;
    case DATABOX_CONTAINER_FLEX_LIST:
        snprintf(buf, bufLen, "{FLEX LIST}");
        break;
    case DATABOX_CONTAINER_FLEX_SET:
        snprintf(buf, bufLen, "{FLEX SET}");
        break;
    case DATABOX_CONTAINER_FLEX_TUPLE:
        snprintf(buf, bufLen, "{FLEX TUPLE}");
        break;
    case DATABOX_CONTAINER_CFLEX_MAP:
        snprintf(buf, bufLen, "{CFLEX MAP}");
        break;
    case DATABOX_CONTAINER_CFLEX_LIST:
        snprintf(buf, bufLen, "{CFLEX LIST}");
        break;
    case DATABOX_CONTAINER_CFLEX_SET:
        snprintf(buf, bufLen, "{CFLEX SET}");
        break;
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        snprintf(buf, bufLen, "{CFLEX TUPLE}");
        break;
    default:
        snprintf(buf, bufLen, "{INVALID TYPE!}");
        break;
    }

    return buf;
}

void databoxReprSay(const char *msg, const databox *const box) {
    printf("%s %s\n", msg, databoxReprStr(box));
}

void databoxRepr(const databox *const box) {
    printf("%s", databoxReprStr(box));
}

#define CTEST_INCLUDE_KEYGEN
#include "ctest.h"
#include "perf.h"

int databoxTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    assert(sizeof(databox) == 16);

    TEST("starting numbers don't ruin the sort") {
        const databox keybox = databoxNewBytesAllowEmbed("120abc", 6);
        const databox keyboxZ = databoxNewBytesString("120zzz");
        const int compared = databoxCompare(&keybox, &keyboxZ);
        if (compared >= 0) {
            ERR("Bad sort! Compared: %d\n", compared);
        }
    }

    TEST("compare forces ordering") {
        databox lowest = databoxVoid();
        const size_t loopers = 1ULL << 23;
        PERF_TIMERS_SETUP;
        for (size_t j = 0; j < loopers; j++) {
            char *key = genkey("45key", j);
            char *key100 = genkey("45key", j * 100 + j * 10 + 9);
            const databox keybox = databoxNewBytesAllowEmbed(key, strlen(key));
            const databox keybox100 = databoxNewBytesString(key);
            if (databoxCompare(&lowest, &keybox) > 0) {
                ERR("Bad sort at key %s\n", key);
            }

            PERF_TIMERS_STAT_START;
            if (databoxCompare(&keybox, &keybox100) > 0) {
                ERR("key * 100 is smaller than key for %s %s\n", key, key100);
            }

            PERF_TIMERS_STAT_STOP(j);

            lowest = keybox;
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(loopers, "compares");
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
