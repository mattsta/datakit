#include "databox.h"
#include "floatExtended.h"
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
        // cppcheck-suppress invalidPointerCast - intentional type-punning to
        // get byte representation
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
    /* @own: container; free=databoxFreeData */
    box->data.bytes.start = zmalloc(len);
    memcpy(box->data.bytes.start, ptr, len);
}

/* A more pre-prepared version of databoxRetainBytesSelf() where the
 * memory size determination has been done out of band of databox.
 * This version also pre-supposes you're only passing in type DATABOX_BYTES.
 * OWNERSHIP: 'bytes' is handed to the box (allocated = true), so a later
 * databoxFreeData() frees it — pass heap memory the box may keep. */
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
 * Returns:
 *   >= 0: the 'cache' slot now holding the bytes.  The CACHE owns that
 *         memory (the box borrows it: 'allocated' stays false, so a
 *         later databoxFreeData() will not free the cache's buffer);
 *         the caller must keep the cache alive while the box is live.
 *   -1:   bytes retained privately (embedded, or freshly allocated with
 *         the box owning the allocation via 'allocated' = true).
 *   -2:   the box type does not reference external bytes; untouched. */
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
        /* Find the smallest populated size class that fits; each slot
         * doubles the previous slot's capacity. */
        for (ssize_t slot = 0; slot < (ssize_t)COUNT_ARRAY(cache->bytes);
             slot++, start *= 2) {
            if (dst->len <= start) {
                if (cache->bytes[slot]) {
                    /* Borrow the cache's buffer: the cache retains
                     * ownership, so the box must NOT claim it (a
                     * claimed borrow would let databoxFreeData free
                     * the cache slot out from under the cache). */
                    dst->data.bytes.start = cache->bytes[slot];
                    memcpy(dst->data.bytes.start, src, dst->len);
                    return slot;
                }

                /* fitting slot is unpopulated: retain privately below */
                break;
            }
        }

        /* No usable cache slot (unpopulated, or larger than the largest
         * size class): retain through a private allocation the box
         * owns; the bytes are ALWAYS retained on this path. */
        /* @own: container; free=databoxFreeData */
        dst->data.bytes.start = zmalloc(dst->len);
        dst->allocated = true;
        memcpy(dst->data.bytes.start, src, dst->len);
        return -1;
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
            /* @own: container; free=databoxFreeData */
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
        /* @own: container; free=databoxFreeData */
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
        /* @own: container; free=databoxFreeData */
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

/* Zero-init in every constructor: the flag bits (.allocated, .created,
 * .big) are contract state read by databoxFreeData and copy helpers, so
 * they must never carry stack garbage. */
databox databoxNewBytes(const void *ptr, size_t len) {
    databox box = {{0}};
    box.type = DATABOX_BYTES;
    box.data.bytes.custart = ptr;
    box.len = len;
    return box;
}

databox databoxNewBytesAllowEmbed(const void *ptr, size_t len) {
    databox box = {{0}};
    databoxUpdateBytesAllowEmbed(&box, ptr, len);
    return box;
}

databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len) {
    databox box = {{0}};
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
    databox box = {{0}};
    databoxUpdateOffsetAllowEmbed(&box, ptr, offset, len);
    return box;
}

databox databoxNewBytesString(const char *str) {
    return databoxNewBytes(str, strlen(str));
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
    /* Natural-sort byte comparison: digit RUNS compare numerically
     * (longer run is the larger number, then lex on the digit
     * bytes); everything else compares lexicographically.  This
     * produces a strict total order so it is safe as a binary-search
     * comparator.
     *
     * The previous formulation tracked only the FIRST digit
     * difference between a and b and then made post-hoc decisions
     * when one side's digit run ended before the other.  That
     * produced a non-transitive ordering: comparing the triplet
     * { "g_2_39", "g_10_0", "g_1_6" } reported A<B, B>C, A>C — a
     * cycle.  Binary search over a flex sorted by such a comparator
     * silently dropped keys that iteration could still find,
     * because the search walked a wrong branch.
     *
     * Run-length-first comparison is transitive: digit runs of
     * different length are ordered by length alone, runs of the
     * same length fall back to memcmp.  Non-digit positions are
     * pure byte comparison, which is trivially transitive. */
    size_t ai = 0;
    size_t bi = 0;
    while (ai < aLen && bi < bLen) {
        const uint_fast8_t aa = aBytes[ai];
        const uint_fast8_t bb = bBytes[bi];
        const bool aIsDigit = StrIsdigit(aa);
        const bool bIsDigit = StrIsdigit(bb);
        if (aIsDigit && bIsDigit) {
            size_t aEnd = ai;
            while (aEnd < aLen && StrIsdigit(aBytes[aEnd])) {
                aEnd++;
            }
            size_t bEnd = bi;
            while (bEnd < bLen && StrIsdigit(bBytes[bEnd])) {
                bEnd++;
            }
            const size_t aRun = aEnd - ai;
            const size_t bRun = bEnd - bi;
            if (aRun != bRun) {
                return aRun < bRun ? -1 : 1;
            }
            const int cmp = memcmp(aBytes + ai, bBytes + bi, aRun);
            if (cmp != 0) {
                return cmp < 0 ? -1 : 1;
            }
            ai = aEnd;
            bi = bEnd;
            continue;
        }
        if (aa < bb) {
            return -1;
        }
        if (aa > bb) {
            return 1;
        }
        ai++;
        bi++;
    }
    if (ai < aLen) {
        return 1;
    }
    if (bi < bLen) {
        return -1;
    }
    return 0;
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

    /* Use the portable comparison helpers from floatExtended.h */
    if (aIsUnsigned) {
        return dk_compare_uint64_double(a->data.u, r);
    }

    return dk_compare_int64_double(a->data.i, r);
}

DK_INLINE_ALWAYS int databoxCompareInt128Float(const databox *a,
                                               const databox *b) {
    assert(IS_INTEGER(a) && DATABOX_IS_FLOAT(b));

    const bool aIsUnsigned =
        (a->type == DATABOX_UNSIGNED_64) || (a->type == DATABOX_UNSIGNED_128);
    const double r =
        b->type == DATABOX_FLOAT_32 ? (double)b->data.f32 : b->data.d64;

#if DK_HAS_FLOAT_EXTENDED
    /* Extended precision path - can directly compare 128-bit integers with
     * floats because dk_float_extended has enough mantissa bits. */
    if (aIsUnsigned) {
        const __uint128_t ua =
            (a->type == DATABOX_UNSIGNED_64) ? a->data.u : *a->data.u128;
        const dk_float_extended x = (dk_float_extended)ua;
        const dk_float_extended y = (dk_float_extended)r;
        _gtlteqReturn(x, y);
    }

    const __int128_t ia =
        (a->type == DATABOX_SIGNED_64) ? a->data.i : *a->data.i128;
    const dk_float_extended x = (dk_float_extended)ia;
    const dk_float_extended y = (dk_float_extended)r;
    _gtlteqReturn(x, y);
#else
    /* Fallback path - no extended precision available.
     * We convert the float to __int128 and compare integer parts.
     * This may lose fractional precision but maintains consistent ordering. */
    if (aIsUnsigned) {
        /* Negative double is always less than unsigned */
        if (r < 0) {
            return 1; /* a > b */
        }

        /* Check if double exceeds uint128 range */
        if (r > (double)UINT128_MAX) {
            return -1; /* a < b */
        }

        const __uint128_t ua =
            (a->type == DATABOX_UNSIGNED_64) ? a->data.u : *a->data.u128;

        /* Truncate double to integer for comparison.
         * For ordering consistency, we compare integer parts first,
         * then handle fractional part to break ties. */
        const __uint128_t r_truncated = (__uint128_t)r;

        if (ua < r_truncated) {
            return -1;
        }

        if (ua > r_truncated) {
            return 1;
        }

        /* Integer parts equal - check fractional part */
        const double frac = r - (double)r_truncated;
        if (frac > 0.0) {
            return -1; /* a < b because b has positive fractional part */
        }

        return 0;
    }

    /* Signed path */
    if (r < (double)INT128_MIN) {
        return 1; /* a > b */
    }

    if (r > (double)INT128_MAX) {
        return -1; /* a < b */
    }

    const __int128_t ia =
        (a->type == DATABOX_SIGNED_64) ? a->data.i : *a->data.i128;

    const __int128_t r_truncated = (__int128_t)r;

    if (ia < r_truncated) {
        return -1;
    }

    if (ia > r_truncated) {
        return 1;
    }

    /* Integer parts equal - check fractional part */
    const double frac = r - (double)r_truncated;
    if (frac > 0.0) {
        return -1; /* a < b */
    }

    if (frac < 0.0) {
        return 1; /* a > b */
    }

    return 0;
#endif
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
            printf("Attempted to compare: %" PRIu32 " and %" PRIu32 "?",
                   (uint32_t)a->type, (uint32_t)b->type);
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
        /* Type-order architecture: the NUMERIC class cross-compares by
         * value (the jump table above); everything else compares equal
         * only to its own type and mismatches sort by the global type
         * order (below). DATABOX_PTR is a deliberate exception that
         * JOINS the numeric class as DATABOX_UNSIGNED_64 (raw pointer
         * bits): flex storage already coerces PTR to unsigned, so a
         * stored-then-decoded pointer comes back UNSIGNED_64 and must
         * order identically against its own live PTR box or a
         * container disagrees with itself. Joining the existing
         * integer total order (rather than PTR-only special cases)
         * keeps mixed comparisons transitive. */
        const bool aPtr = DATABOX_IS_PTR(a);
        const bool bPtr = DATABOX_IS_PTR(b);
        if ((aPtr || IS_INTEGER(a)) && (bPtr || IS_INTEGER(b))) {
            if (!aPtr && !bPtr) {
                return databoxCompareInteger(a, b);
            }

            databox aa = *a;
            databox bb = *b;
            if (aPtr) {
                aa.type = DATABOX_UNSIGNED_64;
            }
            if (bPtr) {
                bb.type = DATABOX_UNSIGNED_64;
            }
            return databoxCompareInteger(&aa, &bb);
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
#include "cleaner.h"
#include <stdio.h> /* printf */

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

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

    TEST("retain cache: borrow vs own vs embed ownership") {
        static const char payload[] =
            "this payload is comfortably longer than the embed threshold";
        const size_t payloadLen = sizeof(payload) - 1;

        /* Slot path: box BORROWS cache memory (allocated stays false),
         * so FreeData must not free the cache's buffer. */
        databoxRetainCache cache = {{0}};
        /* @own: cache */
        cache.bytes[0] = zmalloc(128);
        uint8_t *const slot0 = cache.bytes[0];

        databox borrowed = databoxNewBytes(payload, payloadLen);
        assert(databoxRetainBytesSelf(&borrowed, &cache) == 0);
        assert(!borrowed.allocated);
        assert(borrowed.data.bytes.start == slot0);
        assert(memcmp(borrowed.data.bytes.start, payload, payloadLen) == 0);
        databoxFreeData(&borrowed); /* no-op: cache still owns slot0 */
        assert(cache.bytes[0] == slot0);
        assert(memcmp(slot0, payload, payloadLen) == 0);

        /* Unpopulated-slot path: private allocation the box OWNS. */
        databoxRetainCache empty = {{0}};
        databox owned = databoxNewBytes(payload, payloadLen);
        assert(databoxRetainBytesSelf(&owned, &empty) == -1);
        assert(owned.allocated);
        assert(owned.data.bytes.start != (const void *)payload);
        assert(memcmp(owned.data.bytes.start, payload, payloadLen) == 0);
        databoxFreeData(&owned); /* frees the private retain (ASAN checks) */

        /* Oversized path (> largest 4 MiB size class): still retained. */
        const size_t bigLen = (4 * 1024 * 1024) + 64;
        cleanerName(zfree, U8) uint8_t *big = zmalloc(bigLen);
        memset(big, 0x5A, bigLen);
        databox huge = databoxNewBytes(big, bigLen);
        assert(databoxRetainBytesSelf(&huge, &cache) == -1);
        assert(huge.allocated);
        assert(huge.data.bytes.start != big);
        assert(((const uint8_t *)huge.data.bytes.start)[bigLen - 1] == 0x5A);
        databoxFreeData(&huge);

        /* Embed path: tiny bytes embed, no cache use, nothing to free. */
        databox tiny = databoxNewBytes("hi", 2);
        assert(databoxRetainBytesSelf(&tiny, &cache) == -1);
        assert(tiny.type == DATABOX_BYTES_EMBED && !tiny.allocated);

        /* Non-bytes types are untouched. */
        databox num = databoxNewSigned(42);
        assert(databoxRetainBytesSelf(&num, &cache) == -2);
        assert(num.data.i == 42);

        zfree(slot0);
    }

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

    TEST("comparison reflexivity - same value equals itself") {
        /* Test signed integers at boundaries */
        int64_t signedVals[] = {INT64_MIN, INT64_MIN + 1, -1000000, -1, 0, 1,
                                1000000,   INT64_MAX - 1, INT64_MAX};
        for (size_t i = 0; i < sizeof(signedVals) / sizeof(signedVals[0]);
             i++) {
            databox a = databoxNewSigned(signedVals[i]);
            if (databoxCompare(&a, &a) != 0) {
                ERR("Reflexivity failed for signed %" PRId64, signedVals[i]);
            }
        }

        /* Test unsigned integers */
        uint64_t unsignedVals[] = {0, 1, 1000000, UINT64_MAX - 1, UINT64_MAX};
        for (size_t i = 0; i < sizeof(unsignedVals) / sizeof(unsignedVals[0]);
             i++) {
            databox a = databoxNewUnsigned(unsignedVals[i]);
            if (databoxCompare(&a, &a) != 0) {
                ERR("Reflexivity failed for unsigned %" PRIu64,
                    unsignedVals[i]);
            }
        }

        /* Test floats/doubles */
        double realVals[] = {-1e38, -1e10, -1.0, -0.5, 0.0,
                             0.5,   1.0,   1e10, 1e38};
        for (size_t i = 0; i < sizeof(realVals) / sizeof(realVals[0]); i++) {
            databox a = databoxNewReal(realVals[i]);
            if (databoxCompare(&a, &a) != 0) {
                ERR("Reflexivity failed for real %f", realVals[i]);
            }
        }

        /* Test bytes */
        const char *strings[] = {"", "a", "ab", "abc", "hello world"};
        for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
            databox a = databoxNewBytesString(strings[i]);
            if (databoxCompare(&a, &a) != 0) {
                ERR("Reflexivity failed for string '%s'", strings[i]);
            }
        }
    }

    TEST("comparison anti-symmetry - if a < b then b > a") {
        /* Test signed integers */
        databox a = databoxNewSigned(-100);
        databox b = databoxNewSigned(100);
        int cmpAB = databoxCompare(&a, &b);
        int cmpBA = databoxCompare(&b, &a);
        if (!((cmpAB < 0 && cmpBA > 0) || (cmpAB > 0 && cmpBA < 0) ||
              (cmpAB == 0 && cmpBA == 0))) {
            ERR("Anti-symmetry failed for signed: %d vs %d", cmpAB, cmpBA);
        }

        /* Test with boundaries */
        a = databoxNewSigned(INT64_MIN);
        b = databoxNewSigned(INT64_MAX);
        cmpAB = databoxCompare(&a, &b);
        cmpBA = databoxCompare(&b, &a);
        if (!(cmpAB < 0 && cmpBA > 0)) {
            ERR("Anti-symmetry failed at boundaries: %d vs %d", cmpAB, cmpBA);
        }

        /* Test strings */
        a = databoxNewBytesString("apple");
        b = databoxNewBytesString("banana");
        cmpAB = databoxCompare(&a, &b);
        cmpBA = databoxCompare(&b, &a);
        if (!(cmpAB < 0 && cmpBA > 0)) {
            ERR("Anti-symmetry failed for strings: %d vs %d", cmpAB, cmpBA);
        }
    }

    TEST("comparison transitivity - if a < b and b < c then a < c") {
        /* Test with signed integers */
        databox a = databoxNewSigned(-1000);
        databox b = databoxNewSigned(0);
        databox c = databoxNewSigned(1000);
        if (!(databoxCompare(&a, &b) < 0 && databoxCompare(&b, &c) < 0 &&
              databoxCompare(&a, &c) < 0)) {
            ERRR("Transitivity failed for signed integers");
        }

        /* Test with extreme values */
        a = databoxNewSigned(INT64_MIN);
        b = databoxNewSigned(0);
        c = databoxNewSigned(INT64_MAX);
        if (!(databoxCompare(&a, &b) < 0 && databoxCompare(&b, &c) < 0 &&
              databoxCompare(&a, &c) < 0)) {
            ERRR("Transitivity failed at boundaries");
        }

        /* Test with strings */
        a = databoxNewBytesString("aaa");
        b = databoxNewBytesString("bbb");
        c = databoxNewBytesString("ccc");
        if (!(databoxCompare(&a, &b) < 0 && databoxCompare(&b, &c) < 0 &&
              databoxCompare(&a, &c) < 0)) {
            ERRR("Transitivity failed for strings");
        }

        /* Test with floats */
        databox f1 = databoxNewReal(-100.0f);
        databox f2 = databoxNewReal(0.0f);
        databox f3 = databoxNewReal(100.0f);
        if (!(databoxCompare(&f1, &f2) < 0 && databoxCompare(&f2, &f3) < 0 &&
              databoxCompare(&f1, &f3) < 0)) {
            ERRR("Transitivity failed for floats");
        }
    }

    TEST("signed vs unsigned integer comparison consistency") {
        /* Positive values should compare equal regardless of signed/unsigned */
        databox s = databoxNewSigned(100);
        databox u = databoxNewUnsigned(100);
        if (databoxCompare(&s, &u) != 0) {
            ERRR("Same positive value differs between signed/unsigned");
        }

        /* Zero */
        s = databoxNewSigned(0);
        u = databoxNewUnsigned(0);
        if (databoxCompare(&s, &u) != 0) {
            ERRR("Zero differs between signed/unsigned");
        }

        /* Negative signed should be less than any unsigned */
        s = databoxNewSigned(-1);
        u = databoxNewUnsigned(0);
        if (databoxCompare(&s, &u) >= 0) {
            ERRR("Negative signed should be less than unsigned 0");
        }

        /* Large unsigned values */
        s = databoxNewSigned(INT64_MAX);
        u = databoxNewUnsigned((uint64_t)INT64_MAX + 1);
        if (databoxCompare(&s, &u) >= 0) {
            ERRR("INT64_MAX should be less than INT64_MAX+1 unsigned");
        }
    }

    TEST("integer vs float comparison consistency") {
        /* Equal values */
        databox ibox = databoxNewSigned(100);
        databox fbox = databoxNewReal(100.0f);
        if (databoxCompare(&ibox, &fbox) != 0) {
            ERRR("100 int should equal 100.0 float");
        }

        /* Integer less than float */
        ibox = databoxNewSigned(99);
        fbox = databoxNewReal(99.5f);
        if (databoxCompare(&ibox, &fbox) >= 0) {
            ERRR("99 int should be less than 99.5 float");
        }

        /* Integer greater than float */
        ibox = databoxNewSigned(100);
        fbox = databoxNewReal(99.5f);
        if (databoxCompare(&ibox, &fbox) <= 0) {
            ERRR("100 int should be greater than 99.5 float");
        }

        /* Negative values */
        ibox = databoxNewSigned(-50);
        fbox = databoxNewReal(-50.0);
        if (databoxCompare(&ibox, &fbox) != 0) {
            ERRR("-50 int should equal -50.0 double");
        }
    }

    TEST("float vs double comparison consistency") {
        databox f = databoxNewReal(1.5f);
        databox d = databoxNewReal(1.5);
        if (databoxCompare(&f, &d) != 0) {
            ERRR("1.5 float should equal 1.5 double");
        }

        f = databoxNewReal(1.0f);
        d = databoxNewReal(1.1);
        if (databoxCompare(&f, &d) >= 0) {
            ERRR("1.0 float should be less than 1.1 double");
        }

        f = databoxNewReal(-100.5f);
        d = databoxNewReal(-100.5);
        if (databoxCompare(&f, &d) != 0) {
            ERRR("-100.5 float should equal -100.5 double");
        }
    }

    TEST("string comparison with numeric prefixes (natural sort)") {
        /* databox uses natural sort where numeric strings compare by value */
        databox s1 = databoxNewBytesString("10");
        databox s2 = databoxNewBytesString("2");
        /* "10" > "2" in natural sort because 10 > 2 numerically */
        if (databoxCompare(&s1, &s2) <= 0) {
            ERRR("'10' should be greater than '2' in natural sort");
        }

        s1 = databoxNewBytesString("100");
        s2 = databoxNewBytesString("99");
        /* "100" > "99" in natural sort because 100 > 99 numerically */
        if (databoxCompare(&s1, &s2) <= 0) {
            ERRR("'100' should be greater than '99' in natural sort");
        }

        /* Same prefix, different length (non-numeric) */
        s1 = databoxNewBytesString("abc");
        s2 = databoxNewBytesString("abcd");
        if (databoxCompare(&s1, &s2) >= 0) {
            ERRR("'abc' should be less than 'abcd'");
        }

        /* Verify natural sort with mixed content */
        s1 = databoxNewBytesString("file2.txt");
        s2 = databoxNewBytesString("file10.txt");
        /* "file2.txt" < "file10.txt" because 2 < 10 */
        if (databoxCompare(&s1, &s2) >= 0) {
            ERRR(
                "'file2.txt' should be less than 'file10.txt' in natural sort");
        }
    }

    TEST("natural sort comparator is a strict total order") {
        /* Regression: a previous formulation of databoxCompareBytes_
         * recorded only the FIRST differing digit and made post-hoc
         * decisions when the digit runs ended at different
         * positions.  That produced a non-transitive ordering for
         * mixed-length numeric keys, which silently broke binary
         * search inside multimapFull (BinarySearch missed keys that
         * iteration found).  Lock the fix here. */
        const char *cases[] = {
            "g_2_39",  "g_10_0", "g_1_6",  "g_0_2",  "g_0_45",
            "g_10_47", "g_1_47", "g_2_40", "g_2_42",
        };
        const size_t n = sizeof(cases) / sizeof(cases[0]);
        databox boxes[9];
        for (size_t i = 0; i < n; i++) {
            boxes[i] = databoxNewBytesString(cases[i]);
        }
        /* Transitivity:  for every ordered triple (i, j, k) the
         * comparator must agree:  if i<j and j<k then i<k.
         * Equivalent: there must exist a permutation of indices
         * such that sort order is monotonic against the
         * comparator. */
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                for (size_t k = 0; k < n; k++) {
                    const int ij = databoxCompare(&boxes[i], &boxes[j]);
                    const int jk = databoxCompare(&boxes[j], &boxes[k]);
                    const int ik = databoxCompare(&boxes[i], &boxes[k]);
                    if (ij < 0 && jk < 0 && ik >= 0) {
                        printf("transitivity broken: %s < %s and %s < %s "
                               "but %s vs %s = %d\n",
                               cases[i], cases[j], cases[j], cases[k], cases[i],
                               cases[k], ik);
                        ERRR("natural-sort comparator is not transitive");
                    }
                    if (ij > 0 && jk > 0 && ik <= 0) {
                        printf("transitivity broken: %s > %s and %s > %s "
                               "but %s vs %s = %d\n",
                               cases[i], cases[j], cases[j], cases[k], cases[i],
                               cases[k], ik);
                        ERRR("natural-sort comparator is not transitive");
                    }
                    if (ij == 0 && jk == 0 && ik != 0) {
                        ERRR("natural-sort equality is not transitive");
                    }
                }
            }
        }
        /* Antisymmetry: cmp(a,b) and cmp(b,a) must be opposite signs
         * (or both zero). */
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                const int ab = databoxCompare(&boxes[i], &boxes[j]);
                const int ba = databoxCompare(&boxes[j], &boxes[i]);
                if ((ab == 0) != (ba == 0)) {
                    ERRR(
                        "natural-sort antisymmetry violated (zero vs nonzero)");
                }
                if (ab > 0 && ba >= 0) {
                    ERRR("natural-sort antisymmetry violated (both positive)");
                }
                if (ab < 0 && ba <= 0) {
                    ERRR("natural-sort antisymmetry violated (both negative)");
                }
            }
        }
        /* Specific orderings the natural sort must preserve. */
        databox a = databoxNewBytesString("g_2_39");
        databox b = databoxNewBytesString("g_10_0");
        databox c = databoxNewBytesString("g_1_6");
        if (!(databoxCompare(&c, &a) < 0)) {
            ERRR("'g_1_6' must be less than 'g_2_39' (1 < 2)");
        }
        if (!(databoxCompare(&a, &b) < 0)) {
            ERRR("'g_2_39' must be less than 'g_10_0' (2 < 10)");
        }
        if (!(databoxCompare(&c, &b) < 0)) {
            ERRR("'g_1_6' must be less than 'g_10_0' (1 < 10)");
        }
    }

    TEST("natural sort handles leading zeros consistently") {
        /* Different leading-zero counts on numerically-equal numbers
         * must produce a stable total order (longer run = larger,
         * preserved across calls). */
        databox a = databoxNewBytesString("001");
        databox b = databoxNewBytesString("1");
        const int ab = databoxCompare(&a, &b);
        const int ba = databoxCompare(&b, &a);
        if (ab == 0) {
            /* Equal is acceptable IF the comparator is consistent.
             * Verify ba is also zero. */
            if (ba != 0) {
                ERRR("leading-zero compare asymmetric");
            }
        } else if ((ab > 0 && ba < 0) || (ab < 0 && ba > 0)) {
            /* Good: opposite signs. */
        } else {
            ERRR("leading-zero compare has same sign in both directions");
        }
        databox c = databoxNewBytesString("0001");
        const int ac = databoxCompare(&a, &c);
        const int ca = databoxCompare(&c, &a);
        if ((ac > 0 && ca >= 0) || (ac < 0 && ca <= 0)) {
            ERRR("zero-run length compare violates antisymmetry");
        }
    }

    TEST("natural sort total order on random key set") {
        /* Generate keys with the same shape as the BUG-A
         * reproduction (g_<N>_<M>) and verify that sorting them
         * with the comparator produces a sequence that is monotonic
         * under the comparator — i.e. no element compares less than
         * any earlier element in the sorted output. */
        char buf[600][16];
        databox boxes[600];
        size_t n = 0;
        for (int it = 0; it < 12; it++) {
            for (int k = 0; k < 50; k++) {
                snprintf(buf[n], sizeof(buf[n]), "g_%d_%d", it, k);
                boxes[n] = databoxNewBytesString(buf[n]);
                n++;
            }
        }
        /* Insertion sort (stable, O(n^2) but n=600 is fine for a
         * test). */
        for (size_t i = 1; i < n; i++) {
            databox key = boxes[i];
            char *kbuf = buf[i];
            size_t j = i;
            char tmpkey[16];
            while (j > 0 && databoxCompare(&boxes[j - 1], &key) > 0) {
                boxes[j] = boxes[j - 1];
                /* shift buf in parallel so debug prints stay
                 * aligned.  We only need it for the error path. */
                memcpy(tmpkey, buf[j - 1], sizeof(tmpkey));
                memcpy(buf[j], tmpkey, sizeof(tmpkey));
                j--;
            }
            boxes[j] = key;
            memcpy(buf[j], kbuf, sizeof(buf[j]));
        }
        /* Verify monotonic. */
        for (size_t i = 1; i < n; i++) {
            if (databoxCompare(&boxes[i - 1], &boxes[i]) > 0) {
                printf("sort produced non-monotonic: idx %zu '%s' > %zu '%s'\n",
                       i - 1, buf[i - 1], i, buf[i]);
                ERRR("natural sort produces non-monotonic ordering");
            }
        }
    }

    TEST("empty string comparisons") {
        databox empty = databoxNewBytesString("");
        databox nonEmpty = databoxNewBytesString("a");
        if (databoxCompare(&empty, &nonEmpty) >= 0) {
            ERRR("Empty string should be less than any non-empty string");
        }

        databox empty2 = databoxNewBytesString("");
        if (databoxCompare(&empty, &empty2) != 0) {
            ERRR("Empty strings should be equal");
        }
    }

    TEST("embedded vs non-embedded bytes comparison") {
        /* Short string that can be embedded */
        const char *shortStr = "hello";
        databox embed = databoxNewBytesAllowEmbed(shortStr, strlen(shortStr));
        databox noEmbed = databoxNewBytes(shortStr, strlen(shortStr));
        if (databoxCompare(&embed, &noEmbed) != 0) {
            ERRR("Embedded and non-embedded same string should be equal");
        }

        /* Long string that won't be embedded */
        const char *longStr = "this is a longer string that cannot be embedded";
        databox long1 = databoxNewBytesString(longStr);
        databox long2 = databoxNewBytesAllowEmbed(longStr, strlen(longStr));
        if (databoxCompare(&long1, &long2) != 0) {
            ERRR("Long strings should compare equal regardless of type");
        }
    }

    TEST("boundary value sorting consistency") {
        /* Create array of boundary values and verify sort order */
        databox boxes[10];
        int idx = 0;
        boxes[idx++] = databoxNewSigned(INT64_MIN);
        boxes[idx++] = databoxNewSigned(-1);
        boxes[idx++] = databoxNewSigned(0);
        boxes[idx++] = databoxNewUnsigned(0);
        boxes[idx++] = databoxNewSigned(1);
        boxes[idx++] = databoxNewUnsigned(1);
        boxes[idx++] = databoxNewSigned(INT64_MAX);
        boxes[idx++] = databoxNewUnsigned((uint64_t)INT64_MAX);
        boxes[idx++] = databoxNewUnsigned((uint64_t)INT64_MAX + 1);
        boxes[idx++] = databoxNewUnsigned(UINT64_MAX);

        /* Verify ascending order */
        for (int i = 0; i < idx - 1; i++) {
            int cmp = databoxCompare(&boxes[i], &boxes[i + 1]);
            if (cmp > 0) {
                ERR("Boundary sort order violated at index %d", i);
            }
        }
    }

    TEST("consistent sort order across many iterations") {
        /* Verify same comparison always produces same result */
        databox a = databoxNewSigned(12345);
        databox b = databoxNewSigned(54321);
        int firstCmp = databoxCompare(&a, &b);
        for (int i = 0; i < 10000; i++) {
            int cmp = databoxCompare(&a, &b);
            if (cmp != firstCmp) {
                ERR("Comparison result changed at iteration %d", i);
            }
        }

        /* Test with strings */
        a = databoxNewBytesString("consistent");
        b = databoxNewBytesString("ordering");
        firstCmp = databoxCompare(&a, &b);
        for (int i = 0; i < 10000; i++) {
            int cmp = databoxCompare(&a, &b);
            if (cmp != firstCmp) {
                ERR("String comparison result changed at iteration %d", i);
            }
        }
    }

    TEST("binary search simulation - integers in sorted array") {
        /* Simulate binary search with databox comparisons */
        const int N = 1000;
        databox sorted[N];
        for (int i = 0; i < N; i++) {
            sorted[i] = databoxNewSigned(i * 10); /* 0, 10, 20, ... */
        }

        /* Search for existing values */
        for (int target = 0; target < N * 10; target += 10) {
            databox tbox = databoxNewSigned(target);
            int lo = 0, hi = N - 1;
            bool found = false;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int cmp = databoxCompare(&tbox, &sorted[mid]);
                if (cmp == 0) {
                    found = true;
                    break;
                } else if (cmp < 0) {
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (!found) {
                ERR("Binary search failed to find existing value %d", target);
            }
        }

        /* Search for non-existing values */
        for (int target = 5; target < N * 10; target += 10) {
            databox tbox = databoxNewSigned(target);
            int lo = 0, hi = N - 1;
            bool found = false;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int cmp = databoxCompare(&tbox, &sorted[mid]);
                if (cmp == 0) {
                    found = true;
                    break;
                } else if (cmp < 0) {
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (found) {
                ERR("Binary search found non-existing value %d", target);
            }
        }
    }

    TEST("binary search simulation - strings in sorted array") {
        const char *strings[] = {"apple",  "banana",   "cherry", "date",
                                 "elder",  "fig",      "grape",  "honeydew",
                                 "indigo", "jackfruit"};
        const int N = sizeof(strings) / sizeof(strings[0]);
        databox sorted[10];
        for (int i = 0; i < N; i++) {
            sorted[i] = databoxNewBytesString(strings[i]);
        }

        /* Verify array is sorted */
        for (int i = 0; i < N - 1; i++) {
            if (databoxCompare(&sorted[i], &sorted[i + 1]) >= 0) {
                ERR("String array not sorted at index %d", i);
            }
        }

        /* Search for existing values */
        for (int i = 0; i < N; i++) {
            databox tbox = databoxNewBytesString(strings[i]);
            int lo = 0, hi = N - 1;
            bool found = false;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int cmp = databoxCompare(&tbox, &sorted[mid]);
                if (cmp == 0) {
                    found = true;
                    break;
                } else if (cmp < 0) {
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (!found) {
                ERR("Binary search failed to find '%s'", strings[i]);
            }
        }

        /* Search for non-existing values */
        const char *missing[] = {"apricot", "blueberry", "coconut"};
        for (int i = 0; i < 3; i++) {
            databox tbox = databoxNewBytesString(missing[i]);
            int lo = 0, hi = N - 1;
            bool found = false;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int cmp = databoxCompare(&tbox, &sorted[mid]);
                if (cmp == 0) {
                    found = true;
                    break;
                } else if (cmp < 0) {
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (found) {
                ERR("Binary search found non-existing '%s'", missing[i]);
            }
        }
    }

    TEST("mixed type array sorting verification") {
        /* Create array with mixed types and verify databox compares work */
        databox arr[20];
        int idx = 0;
        arr[idx++] = databoxNewSigned(-100);
        arr[idx++] = databoxNewSigned(-50);
        arr[idx++] = databoxNewSigned(-1);
        arr[idx++] = databoxNewSigned(0);
        arr[idx++] = databoxNewSigned(1);
        arr[idx++] = databoxNewSigned(50);
        arr[idx++] = databoxNewSigned(100);
        arr[idx++] = databoxNewUnsigned(100);
        arr[idx++] = databoxNewUnsigned(1000);
        arr[idx++] = databoxNewUnsigned(UINT64_MAX);

        /* Verify ascending order for numeric types */
        for (int i = 0; i < idx - 1; i++) {
            int cmp = databoxCompare(&arr[i], &arr[i + 1]);
            if (cmp > 0) {
                ERR("Mixed numeric sort violated at index %d", i);
            }
        }
    }

    TEST("stress test - random comparisons maintain consistency") {
        /* Generate random values and verify comparison properties */
        srand(12345); /* Fixed seed for reproducibility */
        for (int iter = 0; iter < 1000; iter++) {
            int64_t val1 = (int64_t)rand() - RAND_MAX / 2;
            int64_t val2 = (int64_t)rand() - RAND_MAX / 2;
            databox a = databoxNewSigned(val1);
            databox b = databoxNewSigned(val2);

            int cmpAB = databoxCompare(&a, &b);
            int cmpBA = databoxCompare(&b, &a);
            int cmpAA = databoxCompare(&a, &a);
            int cmpBB = databoxCompare(&b, &b);

            /* Reflexivity */
            if (cmpAA != 0 || cmpBB != 0) {
                ERR("Reflexivity failed in stress test: cmpAA=%d, cmpBB=%d",
                    cmpAA, cmpBB);
            }

            /* Anti-symmetry */
            if (!((cmpAB < 0 && cmpBA > 0) || (cmpAB > 0 && cmpBA < 0) ||
                  (cmpAB == 0 && cmpBA == 0))) {
                ERR("Anti-symmetry failed in stress test: cmpAB=%d, cmpBA=%d",
                    cmpAB, cmpBA);
            }

            /* Consistency with actual values */
            if (val1 < val2 && cmpAB >= 0) {
                ERR("Comparison doesn't match actual values: %" PRId64
                    " vs %" PRId64,
                    val1, val2);
            }
            if (val1 > val2 && cmpAB <= 0) {
                ERR("Comparison doesn't match actual values: %" PRId64
                    " vs %" PRId64,
                    val1, val2);
            }
            if (val1 == val2 && cmpAB != 0) {
                ERR("Comparison doesn't match actual values: %" PRId64
                    " vs %" PRId64,
                    val1, val2);
            }
        }
    }

    TEST("stress test - random string comparisons") {
        srand(54321);
        char buf1[32], buf2[32];
        for (int iter = 0; iter < 1000; iter++) {
            /* Use only alphabetic chars to avoid natural sort complexity */
            int len1 = rand() % 20 + 1;
            int len2 = rand() % 20 + 1;
            for (int i = 0; i < len1; i++) {
                buf1[i] = 'a' + (rand() % 26);
            }
            buf1[len1] = '\0';
            for (int i = 0; i < len2; i++) {
                buf2[i] = 'a' + (rand() % 26);
            }
            buf2[len2] = '\0';

            databox a = databoxNewBytesString(buf1);
            databox b = databoxNewBytesString(buf2);

            int cmpAB = databoxCompare(&a, &b);
            int cmpBA = databoxCompare(&b, &a);

            /* Anti-symmetry */
            if (!((cmpAB < 0 && cmpBA > 0) || (cmpAB > 0 && cmpBA < 0) ||
                  (cmpAB == 0 && cmpBA == 0))) {
                ERR("String anti-symmetry failed in stress test for '%s' vs "
                    "'%s'",
                    buf1, buf2);
            }

            /* For pure alphabetic strings (no digits), should match strcmp */
            int stdCmp = strcmp(buf1, buf2);
            if ((stdCmp < 0 && cmpAB >= 0) || (stdCmp > 0 && cmpAB <= 0) ||
                (stdCmp == 0 && cmpAB != 0)) {
                ERR("Alphabetic comparison doesn't match strcmp for '%s' vs "
                    "'%s'",
                    buf1, buf2);
            }

            /* Verify reflexivity */
            if (databoxCompare(&a, &a) != 0 || databoxCompare(&b, &b) != 0) {
                ERR("Reflexivity failed for '%s' or '%s'", buf1, buf2);
            }
        }
    }

    TEST("large value ranges for binary search correctness") {
        /* Test with values spread across the int64 range */
        int64_t testVals[] = {
            INT64_MIN,    INT64_MIN / 2, -1000000000LL, -1000000LL, -1000LL,
            -1LL,         0LL,           1LL,           1000LL,     1000000LL,
            1000000000LL, INT64_MAX / 2, INT64_MAX};
        const int N = sizeof(testVals) / sizeof(testVals[0]);

        /* Create sorted array */
        databox sorted[13];
        for (int i = 0; i < N; i++) {
            sorted[i] = databoxNewSigned(testVals[i]);
        }

        /* Binary search for each value */
        for (int t = 0; t < N; t++) {
            databox target = databoxNewSigned(testVals[t]);
            int lo = 0, hi = N - 1;
            int foundIdx = -1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int cmp = databoxCompare(&target, &sorted[mid]);
                if (cmp == 0) {
                    foundIdx = mid;
                    break;
                } else if (cmp < 0) {
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (foundIdx != t) {
                ERR("Binary search found at wrong index: expected %d, got %d",
                    t, foundIdx);
            }
        }
    }

    /* ========================================
     * FUZZ TESTS - Oracle-Based Verification
     * ======================================== */
    printf("\n=== DATABOX FUZZ TESTING ===\n\n");

    TEST("FUZZ: all type creation and basic properties") {
        /* Test void/error types */
        databox voidBox = databoxVoid();
        if (voidBox.type != DATABOX_VOID) {
            ERRR("void type incorrect");
        }

        /* Test signed integers at boundaries */
        int64_t signedTests[] = {INT64_MIN, INT64_MIN + 1, -1,       0,
                                 1,         INT64_MAX - 1, INT64_MAX};
        for (size_t i = 0; i < sizeof(signedTests) / sizeof(signedTests[0]);
             i++) {
            databox box = databoxNewSigned(signedTests[i]);
            if (box.type != DATABOX_SIGNED_64 ||
                box.data.i64 != signedTests[i]) {
                ERR("Signed creation failed for %" PRId64, signedTests[i]);
            }
        }

        /* Test unsigned integers at boundaries */
        uint64_t unsignedTests[] = {0, 1, UINT64_MAX - 1, UINT64_MAX};
        for (size_t i = 0; i < sizeof(unsignedTests) / sizeof(unsignedTests[0]);
             i++) {
            databox box = databoxNewUnsigned(unsignedTests[i]);
            if (box.type != DATABOX_UNSIGNED_64 ||
                box.data.u64 != unsignedTests[i]) {
                ERR("Unsigned creation failed for %" PRIu64, unsignedTests[i]);
            }
        }

        /* Test float/double */
        float floatTests[] = {-1e38f, -1.0f, 0.0f, 1.0f, 1e38f};
        for (size_t i = 0; i < sizeof(floatTests) / sizeof(floatTests[0]);
             i++) {
            databox box;
            DATABOX_SET_FLOAT(&box, floatTests[i]);
            if (box.type != DATABOX_FLOAT_32 || box.data.f32 != floatTests[i]) {
                ERR("Float creation failed for %f", floatTests[i]);
            }
        }

        /* Note: databoxNewReal optimizes by storing values that fit in float
         * as FLOAT_32. So we only check extreme values that require DOUBLE_64
         */
        double doubleTests[] = {-1e308, 1e308};
        for (size_t i = 0; i < sizeof(doubleTests) / sizeof(doubleTests[0]);
             i++) {
            databox box = databoxNewReal(doubleTests[i]);
            if (box.type != DATABOX_DOUBLE_64 ||
                box.data.d64 != doubleTests[i]) {
                ERR("Double creation failed for %f", doubleTests[i]);
            }
        }
        /* Test values that fit in float get stored as FLOAT_32 */
        double floatCompatible[] = {-1.0, 0.0, 1.0, 100.0};
        for (size_t i = 0;
             i < sizeof(floatCompatible) / sizeof(floatCompatible[0]); i++) {
            databox box = databoxNewReal(floatCompatible[i]);
            /* These get optimized to FLOAT_32 */
            if (box.type != DATABOX_FLOAT_32) {
                ERR("Float-compatible double creation: expected FLOAT_32 for "
                    "%f",
                    floatCompatible[i]);
            }
        }

        /* Test boolean types */
        databox trueBox = databoxBool(true);
        databox falseBox = databoxBool(false);
        if (trueBox.type != DATABOX_TRUE) {
            ERRR("True type incorrect");
        }
        if (falseBox.type != DATABOX_FALSE) {
            ERRR("False type incorrect");
        }

        /* Test NULL */
        databox nullBox = databoxNull();
        if (nullBox.type != DATABOX_NULL) {
            ERRR("Null type incorrect");
        }

        printf("  verified all basic type creation\n");
    }

    TEST("FUZZ: embedded vs allocated bytes threshold") {
        /* Test bytes at and around the 8-byte embed threshold */
        for (size_t len = 0; len <= 16; len++) {
            char buf[17];
            for (size_t i = 0; i < len; i++) {
                buf[i] = 'a' + (i % 26);
            }
            buf[len] = '\0';

            databox box = databoxNewBytesAllowEmbed(buf, len);

            if (len <= 8) {
                /* Should be embedded */
                if (box.type != DATABOX_BYTES_EMBED) {
                    ERR("Length %zu should be embedded, got type %d", len,
                        box.type);
                }
            } else {
                /* Should be non-embedded bytes */
                if (box.type != DATABOX_BYTES) {
                    ERR("Length %zu should be BYTES, got type %d", len,
                        box.type);
                }
            }

            if (box.len != len) {
                ERR("Length mismatch: expected %zu, got %" PRIu64, len,
                    (uint64_t)box.len);
            }
        }

        printf("  verified embed threshold (0-16 bytes)\n");
    }

    TEST("FUZZ: 128-bit integer operations") {
        /* Test 128-bit signed integers */
        databoxBig bigBox;
        __int128_t signedVals[] = {
            INT128_MIN,    INT128_MIN + 1, (__int128_t)-1, (__int128_t)0,
            (__int128_t)1, INT128_MAX - 1, INT128_MAX};

        for (size_t i = 0; i < sizeof(signedVals) / sizeof(signedVals[0]);
             i++) {
            DATABOX_BIG_SIGNED_128(&bigBox, signedVals[i]);
            if (bigBox.type != DATABOX_SIGNED_128) {
                ERR("128-bit signed type incorrect for index %zu", i);
            }
            if (*bigBox.data.i128 != signedVals[i]) {
                ERR("128-bit signed value mismatch for index %zu", i);
            }
        }

        /* Test 128-bit unsigned integers */
        __uint128_t unsignedVals[] = {(__uint128_t)0, (__uint128_t)1,
                                      UINT128_MAX - 1, UINT128_MAX};

        for (size_t i = 0; i < sizeof(unsignedVals) / sizeof(unsignedVals[0]);
             i++) {
            DATABOX_BIG_UNSIGNED_128(&bigBox, unsignedVals[i]);
            if (bigBox.type != DATABOX_UNSIGNED_128) {
                ERR("128-bit unsigned type incorrect for index %zu", i);
            }
            if (*bigBox.data.u128 != unsignedVals[i]) {
                ERR("128-bit unsigned value mismatch for index %zu", i);
            }
        }

        printf("  verified 128-bit integer operations\n");
    }

    TEST("FUZZ: random signed comparison with oracle") {
        srand(12345);
        const int numTests = 10000;
        int mismatches = 0;

        for (int i = 0; i < numTests; i++) {
            int64_t a = ((int64_t)rand() << 32) | rand();
            int64_t b = ((int64_t)rand() << 32) | rand();

            /* Random sign flip */
            if (rand() % 2) {
                a = -a;
            }
            if (rand() % 2) {
                b = -b;
            }

            databox boxA = databoxNewSigned(a);
            databox boxB = databoxNewSigned(b);

            int cmp = databoxCompare(&boxA, &boxB);

            /* Oracle: simple comparison */
            int expected;
            if (a < b) {
                expected = -1;
            } else if (a > b) {
                expected = 1;
            } else {
                expected = 0;
            }

            /* Verify sign matches */
            if ((cmp < 0) != (expected < 0) || (cmp > 0) != (expected > 0) ||
                (cmp == 0) != (expected == 0)) {
                if (mismatches < 5) {
                    ERR("Signed compare mismatch: %" PRId64 " vs %" PRId64
                        ": got %d expected %d",
                        a, b, cmp, expected);
                }
                mismatches++;
            }
        }

        printf("  %d random signed comparisons, %d mismatches\n", numTests,
               mismatches);
    }

    TEST("FUZZ: random unsigned comparison with oracle") {
        srand(23456);
        const int numTests = 10000;
        int mismatches = 0;

        for (int i = 0; i < numTests; i++) {
            uint64_t a = ((uint64_t)rand() << 32) | rand();
            uint64_t b = ((uint64_t)rand() << 32) | rand();

            databox boxA = databoxNewUnsigned(a);
            databox boxB = databoxNewUnsigned(b);

            int cmp = databoxCompare(&boxA, &boxB);

            /* Oracle */
            int expected;
            if (a < b) {
                expected = -1;
            } else if (a > b) {
                expected = 1;
            } else {
                expected = 0;
            }

            if ((cmp < 0) != (expected < 0) || (cmp > 0) != (expected > 0) ||
                (cmp == 0) != (expected == 0)) {
                if (mismatches < 5) {
                    ERR("Unsigned compare mismatch: %" PRIu64 " vs %" PRIu64
                        ": got %d expected %d",
                        a, b, cmp, expected);
                }
                mismatches++;
            }
        }

        printf("  %d random unsigned comparisons, %d mismatches\n", numTests,
               mismatches);
    }

    TEST("FUZZ: random double comparison with oracle") {
        srand(34567);
        const int numTests = 5000;
        int mismatches = 0;

        for (int i = 0; i < numTests; i++) {
            /* Generate random doubles across a reasonable range */
            double a = ((double)rand() / RAND_MAX - 0.5) * 2e10;
            double b = ((double)rand() / RAND_MAX - 0.5) * 2e10;

            databox boxA = databoxNewReal(a);
            databox boxB = databoxNewReal(b);

            int cmp = databoxCompare(&boxA, &boxB);

            /* Oracle */
            int expected;
            if (a < b) {
                expected = -1;
            } else if (a > b) {
                expected = 1;
            } else {
                expected = 0;
            }

            if ((cmp < 0) != (expected < 0) || (cmp > 0) != (expected > 0) ||
                (cmp == 0) != (expected == 0)) {
                if (mismatches < 5) {
                    ERR("Double compare mismatch: %f vs %f: got %d expected %d",
                        a, b, cmp, expected);
                }
                mismatches++;
            }
        }

        printf("  %d random double comparisons, %d mismatches\n", numTests,
               mismatches);
    }

    TEST("FUZZ: cross-type comparison consistency") {
        /* Compare signed vs unsigned (same value should be equal) */
        for (int64_t val = 0; val < 1000; val++) {
            databox signedBox = databoxNewSigned(val);
            databox unsignedBox = databoxNewUnsigned((uint64_t)val);

            int cmp = databoxCompare(&signedBox, &unsignedBox);
            if (cmp != 0) {
                ERR("Same value %" PRId64
                    " compares unequal across signed/unsigned",
                    val);
            }
        }

        /* Compare float vs double with same value */
        float floatVals[] = {-1000.0f, -1.0f, 0.0f, 1.0f, 1000.0f};
        for (size_t i = 0; i < sizeof(floatVals) / sizeof(floatVals[0]); i++) {
            databox floatBox;
            DATABOX_SET_FLOAT(&floatBox, floatVals[i]);
            databox doubleBox = databoxNewReal((double)floatVals[i]);

            int cmp = databoxCompare(&floatBox, &doubleBox);
            if (cmp != 0) {
                ERR("Same value %f compares unequal across float/double",
                    floatVals[i]);
            }
        }

        printf("  cross-type consistency verified\n");
    }

    TEST("FUZZ: bytes comparison with oracle") {
        srand(45678);
        const int numTests = 5000;
        int mismatches = 0;
        char buf1[64], buf2[64];

        for (int i = 0; i < numTests; i++) {
            /* Generate random lengths */
            int len1 = rand() % 32 + 1;
            int len2 = rand() % 32 + 1;

            /* Fill with alphabetic chars for predictable comparison */
            for (int j = 0; j < len1; j++) {
                buf1[j] = 'a' + (rand() % 26);
            }
            buf1[len1] = '\0';

            for (int j = 0; j < len2; j++) {
                buf2[j] = 'a' + (rand() % 26);
            }
            buf2[len2] = '\0';

            databox boxA = databoxNewBytesString(buf1);
            databox boxB = databoxNewBytesString(buf2);

            int cmp = databoxCompare(&boxA, &boxB);
            int expected = strcmp(buf1, buf2);

            /* Just check sign consistency */
            if ((cmp < 0) != (expected < 0) || (cmp > 0) != (expected > 0) ||
                (cmp == 0) != (expected == 0)) {
                if (mismatches < 5) {
                    ERR("Bytes compare mismatch: '%s' vs '%s': got %d expected "
                        "%d",
                        buf1, buf2, cmp, expected);
                }
                mismatches++;
            }
        }

        printf("  %d random bytes comparisons, %d mismatches\n", numTests,
               mismatches);
    }

    TEST("FUZZ: stress test - 100K random comparisons") {
        srand(99999);
        const int numTests = 100000;
        int errors = 0;

        for (int i = 0; i < numTests; i++) {
            int type = rand() % 4;
            databox boxA, boxB;
            char buf1[16], buf2[16]; /* Must be in loop scope for case 3 */

            switch (type) {
            case 0: {
                /* Signed */
                int64_t a = ((int64_t)rand() << 32) | rand();
                int64_t b = ((int64_t)rand() << 32) | rand();
                if (rand() % 2) {
                    a = -a;
                }
                if (rand() % 2) {
                    b = -b;
                }
                boxA = databoxNewSigned(a);
                boxB = databoxNewSigned(b);
                break;
            }
            case 1: {
                /* Unsigned */
                uint64_t a = ((uint64_t)rand() << 32) | rand();
                uint64_t b = ((uint64_t)rand() << 32) | rand();
                boxA = databoxNewUnsigned(a);
                boxB = databoxNewUnsigned(b);
                break;
            }
            case 2: {
                /* Double */
                double a = ((double)rand() / RAND_MAX - 0.5) * 1e10;
                double b = ((double)rand() / RAND_MAX - 0.5) * 1e10;
                boxA = databoxNewReal(a);
                boxB = databoxNewReal(b);
                break;
            }
            case 3: {
                /* Bytes */
                int len1 = rand() % 15 + 1;
                int len2 = rand() % 15 + 1;
                for (int j = 0; j < len1; j++) {
                    buf1[j] = 'a' + (rand() % 26);
                }
                buf1[len1] = '\0';
                for (int j = 0; j < len2; j++) {
                    buf2[j] = 'a' + (rand() % 26);
                }
                buf2[len2] = '\0';
                boxA = databoxNewBytesString(buf1);
                boxB = databoxNewBytesString(buf2);
                break;
            }
            }

            /* Test properties */
            int cmpAB = databoxCompare(&boxA, &boxB);
            int cmpBA = databoxCompare(&boxB, &boxA);
            int cmpAA = databoxCompare(&boxA, &boxA);

            /* Reflexivity */
            if (cmpAA != 0) {
                errors++;
            }

            /* Anti-symmetry */
            if (!((cmpAB < 0 && cmpBA > 0) || (cmpAB > 0 && cmpBA < 0) ||
                  (cmpAB == 0 && cmpBA == 0))) {
                errors++;
            }
        }

        printf("  100K random comparisons, %d errors\n", errors);
        if (errors > 0) {
            ERR("Stress test had %d errors", errors);
        }
    }

    TEST("compare: DATABOX_PTR orders by value in the integer order") {
        int arena[2] = {0, 0};
        void *lo = &arena[0];
        void *hi = &arena[1];

        const databox pLo = databoxNewPtr(lo);
        const databox pHi = databoxNewPtr(hi);
        const databox uSame = databoxNewUnsigned((uintptr_t)lo);
        const databox sNeg = databoxNewSigned(-1);

        if (databoxCompare(&pLo, &pHi) >= 0 ||
            databoxCompare(&pHi, &pLo) <= 0) {
            ERRR("PTR vs PTR must order by pointer value");
        }
        if (databoxCompare(&pLo, &pLo) != 0) {
            ERRR("PTR must compare equal to itself");
        }
        if (databoxCompare(&pLo, &uSame) != 0 ||
            databoxCompare(&uSame, &pLo) != 0) {
            ERRR("PTR must equal the UNSIGNED box flex decode returns");
        }
        if (databoxCompare(&pLo, &sNeg) <= 0) {
            ERRR("PTR (as unsigned) must order above negative signed");
        }
    }

    printf("\n=== All databox fuzz tests completed! ===\n\n");

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
