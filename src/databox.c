#include "databox.h"
#include "dkDiag.h"
#include "floatExtended.h"
#include "str.h"

#include <math.h> /* isnan */

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
    case DATABOX_BYTES_NEVER_FREE:
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
#if DATABOX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
        *buf = box->data.ptr;
        *len = box->len;
        break;
#endif
    default:
        /* No byte form (booleans, null, 96/128-bit, void, offset) */
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
    case DATABOX_BYTES_NEVER_FREE:
#if DATABOX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
#endif
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
    size_t len = 0;
    if (databoxGetSize(box, &len)) {
        return len;
    }

    switch (box->type) {
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        return sizeof(__uint128_t);
    case DATABOX_PTR:
        return sizeof(void *);
    default:
        return 0; /* markers and other payload-free types */
    }
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

static bool databoxOwnBytes_(databox *box);

/* A more pre-prepared version of databoxRetainBytesSelf() where the
 * memory size determination has been done out of band of databox.
 * This version also pre-supposes you're only passing in type DATABOX_BYTES.
 * OWNERSHIP: 'bytes' (heap memory of at least dst->len bytes) is handed to
 * the box: it holds the copy (allocated = true, freed by a later
 * databoxFreeData()), or, for a value short enough to embed, is freed
 * here. */
void databoxRetainBytesSelfExact(databox *restrict dst, void *restrict bytes) {
    if (dst->len <= sizeof(dst->data.bytes.embed8)) {
        /* Small enough to embed: the box keeps the bytes inline, so the
         * buffer it was handed is not needed and is freed here */
        memcpy(dst->data.bytes.embed8, dst->data.bytes.start, databoxLen(dst));
        dst->type = DATABOX_BYTES_EMBED;
        zfree(bytes);
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
 *   -2:   the box references no external bytes (a non-byte type, bytes
 *         embedded in it, VOID, OFFSET) or an mdsc string / flex
 *         aggregate it already owns; untouched.
 * An mdsc string or embedded flex aggregate it does not own is retained
 * privately under its own type (-1); plain bytes become BYTES. */
ssize_t databoxRetainBytesSelf(databox *restrict dst,
                               databoxRetainCache *restrict cache) {
    /* An mdsc string or an embedded flex aggregate is retained privately
     * under its own type (the cache holds plain bytes only) */
    if (dst->type != DATABOX_BYTES && dst->type != DATABOX_BYTES_NEVER_FREE) {
        return !dst->allocated && databoxOwnBytes_(dst) ? -1 : -2;
    }

    /* Plain bytes: once retained they are BYTES */
    dst->type = DATABOX_BYTES;
    if (dst->len <= sizeof(dst->data.bytes.embed8)) {
        /* If source bytes small enough to become embed, do
         * embed instead of allocating actual memory */
        memcpy(dst->data.bytes.embed8, dst->data.bytes.start, databoxLen(dst));
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

/* 96/128-bit values live behind data.i128 / data.u128, so copies own them
 * the way they own BYTES: in a 16-byte heap block with 'allocated' set,
 * released by databoxFreeData.  'big' marks a live payload pointer, so a
 * heap-owned wide box has big = 1 and every other copy result has
 * big = 0.  A type-only wide box (data.ptr == NULL, as a plain-box decode
 * reports it) stays type-only. */
static bool databoxIsWide_(const databox *box) {
    return box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128;
}

/* Point 'dst' at an owned heap copy of the wide payload 'src' refers to.
 * memcpy reads the payload, so 'src' storage needs no particular
 * alignment.  'dst' and 'src' may be the same box. */
static void databoxOwnWidePayload_(databox *dst, const databox *src) {
    if (!src->data.ptr) {
        dst->data.ptr = NULL;
        dst->allocated = false;
        dst->big = false;
        return;
    }

    /* @own: container; free=databoxFreeData */
    __uint128_t *payload = zmalloc(sizeof(*payload));
    assert(((uintptr_t)payload % alignof(__uint128_t)) == 0);
    memcpy(payload, src->data.ptr, sizeof(*payload));
    dst->data.u128 = payload;
    dst->allocated = true;
    dst->big = true;
}

/* Give 'box' (holding its source's fields) its own copy of the bytes it
 * carries, by type: plain bytes (BYTES, NEVER_FREE) become owned BYTES,
 * embedded when they fit 8 bytes; an embedded flex aggregate is copied to
 * the heap under its own type; an mdsc string becomes a new mdsc.  VOID (a
 * reservation, no contents) and OFFSET (a position in another stream)
 * carry no bytes of their own, nor do the non-byte types: left as they are.
 * True when 'box' now owns a copy. */
static bool databoxOwnBytes_(databox *box) {
    const uint8_t *const bytes = box->data.bytes.start;
    const size_t len = box->len;
    switch (box->type) {
    case DATABOX_BYTES:
    case DATABOX_BYTES_NEVER_FREE:
        if (len <= sizeof(box->data.bytes.embed8)) {
            /* 'bytes' is saved: the embed overlays the pointer */
            box->type = DATABOX_BYTES_EMBED;
            box->allocated = false;
            memcpy(box->data.bytes.embed8, bytes, len);
        } else {
            box->type = DATABOX_BYTES;
            box->allocated = true;
            /* @own: container; free=databoxFreeData */
            box->data.bytes.start = zmalloc(len);
            memcpy(box->data.bytes.start, bytes, len);
        }
        return true;
#if DATABOX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
        box->allocated = true;
        /* @own: container; free=databoxFreeData */
        box->data.ptr = mdscnewlen(bytes, len);
        return true;
#endif
    default:
        return false;
    }
}

void databoxCopyBytesFromBox(databox *dst, const databox *src) {
    *dst = *src;
    if (databoxIsWide_(src)) {
        databoxOwnWidePayload_(dst, src);
        return;
    }

    dst->big = false;
    dst->allocated = false;
    databoxOwnBytes_(dst);
}

bool databoxAllocateIfNeeded(databox *restrict box) {
    /* An owned payload is already independent of its source */
    if (box->allocated) {
        return false;
    }

    if (databoxIsWide_(box)) {
        if (!box->data.ptr) {
            return false;
        }

        databoxOwnWidePayload_(box, box);
        return true;
    }

    return databoxOwnBytes_(box);
}

databox databoxCopy(const databox *restrict src) {
    databox dst;
    databoxCopyBytesFromBox(&dst, src);
    return dst;
}

void databoxBigCopyFrom(databoxBig *dst, const databox *src) {
    /* Read the payload first: 'src' may be 'dst' itself, or point into it. */
    __uint128_t payload = 0;
    const bool wide = databoxIsWide_(src) && src->data.ptr;
    if (wide) {
        memcpy(&payload, src->data.ptr, sizeof(payload));
    }

    if ((const void *)src != (const void *)dst) {
        memcpy(dst, src, sizeof(*src));
    }

    /* 'dst' never owns anything: BYTES keep borrowing from their source
     * and a wide payload lives in dst->extra. */
    dst->allocated = false;
    if (wide) {
        memcpy(dst->extra, &payload, sizeof(payload));
        dst->data.u128 = (__uint128_t *)dst->extra;
        dst->big = true;
    } else {
        dst->big = false;
    }
}

void databoxFreeData(databox *box) {
    if (box) {
        if (box->allocated) {
            /* Owned bytes */
            if (box->type == DATABOX_BYTES) {
                zfree(box->data.ptr);
                box->data.ptr = NULL;
                box->allocated = false;
                box->type = DATABOX_VOID;
            } else if (databoxIsWide_(box)) {
                /* 'allocated' alone says the payload is the box's to
                 * free: DATABOX_BIG_INIT clears it for a value held in
                 * 'extra'.  An address test ("data.ptr is just past the
                 * box") cannot tell a databoxBig's inline value from a
                 * plain box whose heap payload happens to follow it in
                 * memory, as a one-entry rangeBox array and its 16-byte
                 * payload can, so it would leak that payload. */
                zfree(box->data.ptr);
                box->data.ptr = NULL;
                box->allocated = false;
                box->big = false;
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
    databox box = {};
    box.type = DATABOX_BYTES;
    box.data.bytes.custart = ptr;
    box.len = len;
    return box;
}

databox databoxNewBytesAllowEmbed(const void *ptr, size_t len) {
    databox box = {};
    databoxUpdateBytesAllowEmbed(&box, ptr, len);
    return box;
}

databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len) {
    databox box = {};
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
    databox box = {};
    databoxUpdateOffsetAllowEmbed(&box, ptr, offset, len);
    return box;
}

databox databoxNewBytesString(const char *str) {
    return databoxNewBytes(str, strlen(str));
}

databox databoxNewReal(double value) {
    databox box = {};

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

/* True for a float or double box holding NaN. */
static bool databoxIsNaN_(const databox *box) {
    if (box->type == DATABOX_FLOAT_32) {
        return isnan(box->data.f32);
    }

    if (box->type == DATABOX_DOUBLE_64) {
        return isnan(box->data.d64);
    }

    return false;
}

bool databoxEqual(const databox *a, const databox *b) {
    /* Value equality follows IEEE: NaN equals nothing, itself included.
     * databoxCompare is the identity order containers use for keys, where
     * every NaN is one position so a NaN key stays findable. */
    if (databoxIsNaN_(a) || databoxIsNaN_(b)) {
        return false;
    }

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

        [[fallthrough]];
    case c1b(true, true):
        /* Here both values are signed (or we've restricted
         * their values to be compared as signed), so compare
         * as signed: */
        _gtlteqReturn(a->data.i, b->data.i);
    default:
        unreachable();
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

void databoxWidePayloadMissing(const databox *box, const char *where) {
    DK_DIAG_FATAL(
        DK_DIAG_DATABOX_WIDE_DEFERRED,
        "%s: %s box has no payload (a deferred wide value); read "
        "it into wide storage before using it",
        where, box->type == DATABOX_SIGNED_128 ? "SIGNED_128" : "UNSIGNED_128");
}

/* The 96/128-bit comparison arms read the payload of whichever operand is
 * wide, so a deferred operand stops here instead of at a NULL load. */
DK_INLINE_ALWAYS void databoxCompareRequirePayload_(const databox *box) {
    if (unlikely(databoxIsWideDeferred(box))) {
        databoxWidePayloadMissing(box, "databoxCompare");
    }
}

/* An integer of any width as a sign and a magnitude.  Every SIGNED_64,
 * UNSIGNED_64, SIGNED_128 and UNSIGNED_128 value fits: magnitudes reach
 * 2^127 for negatives and 2^128 - 1 for positives, and zero is never
 * negative. */
typedef struct databoxIntegerValue_ {
    __uint128_t magnitude;
    bool negative;
} databoxIntegerValue_;

DK_INLINE_ALWAYS databoxIntegerValue_
databoxIntegerValueOf_(const databox *box) {
    databoxIntegerValue_ v = {};
    switch (box->type) {
    case DATABOX_SIGNED_64:
        v.negative = box->data.i < 0;
        v.magnitude = v.negative ? 0 - (uint64_t)box->data.i : box->data.u;
        break;
    case DATABOX_SIGNED_128: {
        const __int128_t i = *box->data.i128;
        v.negative = i < 0;
        v.magnitude = v.negative ? 0 - (__uint128_t)i : (__uint128_t)i;
        break;
    }
    case DATABOX_UNSIGNED_128:
        v.magnitude = *box->data.u128;
        break;
    default:
        /* UNSIGNED_64, and the boxes that join the unsigned order */
        v.magnitude = box->data.u;
        break;
    }

    return v;
}

DK_INLINE_ALWAYS int databoxIntegerValueCompare_(const databoxIntegerValue_ a,
                                                 const databoxIntegerValue_ b) {
    if (a.negative != b.negative) {
        return a.negative ? -1 : 1;
    }

    /* Same sign: a larger magnitude is larger when positive and smaller
     * when negative. */
    if (a.negative) {
        _gtlteqReturn(b.magnitude, a.magnitude);
    }

    _gtlteqReturn(a.magnitude, b.magnitude);
}

/* Integers of mixed widths, at least one of them 96/128-bit: compare by
 * value as sign and magnitude, so every width and sign pairing is exact. */
DK_INLINE_ALWAYS int databoxCompareInteger128(const databox *a,
                                              const databox *b) {
    databoxCompareRequirePayload_(a);
    databoxCompareRequirePayload_(b);
    return databoxIntegerValueCompare_(databoxIntegerValueOf_(a),
                                       databoxIntegerValueOf_(b));
}

/* The order of reals: the IEEE order, except that NaN sorts above +inf and
 * equals only NaN (every NaN, whatever its sign or payload, is the same
 * key).  -0.0 equals +0.0. */
DK_INLINE_ALWAYS double databoxRealOf_(const databox *box) {
    return box->type == DATABOX_FLOAT_32 ? (double)box->data.f32
                                         : box->data.d64;
}

DK_INLINE_ALWAYS int databoxCompareFloat(const databox *a, const databox *b) {
    const double useA = databoxRealOf_(a);
    const double useB = databoxRealOf_(b);
    const bool aNaN = useA != useA;
    const bool bNaN = useB != useB;
    if (unlikely(aNaN || bNaN)) {
        return (int)aNaN - (int)bNaN;
    }

    _gtlteqReturn(useA, useB);
}

/* An integer of any width against a real, by exact mathematical value: no
 * conversion that can round.  NaN is above every integer. */
DK_INLINE_ALWAYS int databoxCompareIntegerReal_(const databoxIntegerValue_ a,
                                                const double r) {
    if (unlikely(r != r)) {
        return -1;
    }

    /* -0.0 is not below zero, so it takes the non-negative side. */
    const bool rNegative = r < 0;
    if (a.negative != rNegative) {
        return a.negative ? -1 : 1;
    }

    /* Same side of zero: compare the magnitudes.  A magnitude of 2^128 or
     * more (infinity included) is beyond every integer magnitude.  Below
     * that, the conversion to an integer truncates exactly, and the
     * truncated part converts back exactly, so the remainder is the
     * fraction. */
    const double rMagnitude = rNegative ? -r : r;
    int cmp;
    if (rMagnitude >= 0x1p128) {
        cmp = -1;
    } else {
        const __uint128_t whole = (__uint128_t)rMagnitude;
        if (a.magnitude != whole) {
            cmp = a.magnitude < whole ? -1 : 1;
        } else {
            cmp = rMagnitude > (double)whole ? -1 : 0;
        }
    }

    return a.negative ? -cmp : cmp;
}

DK_INLINE_ALWAYS int databoxCompareInt64Float(const databox *a,
                                              const databox *b) {
    assert(IS_INTEGER(a) && DATABOX_IS_FLOAT(b));
    return databoxCompareIntegerReal_(databoxIntegerValueOf_(a),
                                      databoxRealOf_(b));
}

DK_INLINE_ALWAYS int databoxCompareInt128Float(const databox *a,
                                               const databox *b) {
    assert(DATABOX_IS_FLOAT(b));
    databoxCompareRequirePayload_(a);
    return databoxCompareIntegerReal_(databoxIntegerValueOf_(a),
                                      databoxRealOf_(b));
}

/* Both types are below c3bMAX_LT: the numeric jump table. */
DK_INLINE_ALWAYS int databoxCompareNumeric_(const databox *restrict a,
                                            const databox *restrict b) {
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
        /* A VOID box, or a numeric type this table has no entry for: no
         * order exists, so stop in every build (never undefined behaviour
         * where assertions are compiled out) */
        DK_DIAG_FATAL(DK_DIAG_DATABOX_UNCOMPARABLE,
                      "no numeric order between types %" PRIu32 " and %" PRIu32,
                      (uint32_t)a->type, (uint32_t)b->type);
    }
}

/* DATABOX_PTR and DATABOX_CONTAINER_REFERENCE_EXTERNAL join the numeric
 * class as UNSIGNED_64.  flex stores both as unsigned integers, so a stored
 * and decoded pointer comes back UNSIGNED_64 and must order exactly like its
 * live box; and the order stays transitive only if they compare by value
 * against every numeric type, reals and 96/128-bit integers included. */
DK_INLINE_ALWAYS bool databoxJoinsNumeric_(const databox *box) {
    return (box->type >= DATABOX_SIGNED_64 && box->type <= DATABOX_DOUBLE_64) ||
           box->type == DATABOX_PTR ||
           box->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL;
}

int databoxCompare(const databox *restrict a, const databox *restrict b) {
#if 1
    if (IS_BYTES(a) && IS_BYTES(b)) {
        return databoxCompareBytes(a, b);
    }

    /* Verify types of 'a' and 'b' are no larger than 3 bits each. */
    if (a->type < c3bMAX_LT && b->type < c3bMAX_LT) {
        return databoxCompareNumeric_(a, b);
    } else {
        /* Type-order architecture: the numeric class cross-compares by
         * value; everything else compares equal only to its own type, and
         * mismatches sort by the global type order (below). */
        if (databoxJoinsNumeric_(a) && databoxJoinsNumeric_(b)) {
            databox aa = *a;
            databox bb = *b;
            if (aa.type >= c3bMAX_LT) {
                aa.type = DATABOX_UNSIGNED_64;
            }

            if (bb.type >= c3bMAX_LT) {
                bb.type = DATABOX_UNSIGNED_64;
            }

            return databoxCompareNumeric_(&aa, &bb);
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

/* splitmix64's finalizer: spreads every input bit over the result. */
DK_INLINE_ALWAYS uint64_t databoxHashMix_(uint64_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

/* Distinct inputs for the three mixed classes: positive and negative
 * integers outside the int64 range, and reals that are not integers. */
#define DATABOX_HASH_TAG_POSITIVE UINT64_C(0x9E3779B97F4A7C15)
#define DATABOX_HASH_TAG_NEGATIVE UINT64_C(0xC2B2AE3D27D4EB4F)
#define DATABOX_HASH_TAG_REAL UINT64_C(0x165667B19E3779F9)

/* The key of an integer value: its int64 bits inside the int64 range,
 * otherwise a mix of its sign and 128-bit magnitude. */
DK_INLINE_ALWAYS uint64_t databoxIntegerKey_(const databoxIntegerValue_ v) {
    if (!v.negative && v.magnitude <= INT64_MAX) {
        return (uint64_t)v.magnitude;
    }

    if (v.negative && v.magnitude <= ((__uint128_t)1 << 63)) {
        return 0 - (uint64_t)v.magnitude;
    }

    const uint64_t tag =
        v.negative ? DATABOX_HASH_TAG_NEGATIVE : DATABOX_HASH_TAG_POSITIVE;
    return databoxHashMix_(
        (uint64_t)v.magnitude ^
        databoxHashMix_((uint64_t)(v.magnitude >> 64) ^ tag));
}

bool databoxNumericHashKey(const databox *box, uint64_t *key) {
    if (!databoxJoinsNumeric_(box)) {
        return false;
    }

    if (box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128) {
        if (unlikely(databoxIsWideDeferred(box))) {
            databoxWidePayloadMissing(box, "databoxNumericHashKey");
        }
    }

    if (!DATABOX_IS_FLOAT(box)) {
        *key = databoxIntegerKey_(databoxIntegerValueOf_(box));
        return true;
    }

    const double d = databoxRealOf_(box);
    if (d != d) {
        /* Every NaN is one value. */
        *key = databoxHashMix_(UINT64_C(0x7FF8000000000000) ^
                               DATABOX_HASH_TAG_REAL);
        return true;
    }

    /* An integral real within the 128-bit integer range keys as that
     * integer, so it matches every integer box of the same value; -0.0 is
     * the integer 0.  The truncation and its back-conversion are exact, as
     * in databoxCompareIntegerReal_. */
    if (d >= -0x1p127 && d < 0x1p128) {
        const bool negative = d < 0;
        const double magnitude = negative ? -d : d;
        const __uint128_t whole = (__uint128_t)magnitude;
        if ((double)whole == magnitude) {
            const databoxIntegerValue_ v = {.magnitude = whole,
                                            .negative = negative};
            *key = databoxIntegerKey_(v);
            return true;
        }
    }

    /* Any other real (a fraction, an infinity, or beyond the integer
     * range) equals only reals of the same value, which have the same
     * double bits. */
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    *key = databoxHashMix_(bits ^ DATABOX_HASH_TAG_REAL);
    return true;
}

/* Integer numerals beyond 64 bits are 20 to 40 bytes long: 2^64 has 20
 * digits, and INT128_MIN is a '-' and 39 digits. */
#define DATABOX_CANONICAL_WIDE_MIN_LEN 20
#define DATABOX_CANONICAL_WIDE_MAX_LEN 40

bool databoxIntegralRealAsInteger(const databox *in, databox *out) {
    double d;
    if (in->type == DATABOX_DOUBLE_64) {
        d = in->data.d64;
    } else if (in->type == DATABOX_FLOAT_32) {
        d = (double)in->data.f32;
    } else {
        return false;
    }

    /* NaN fails every comparison; -0.0 keeps its sign as a real */
    if (d == 0) {
        if (signbit(d)) {
            return false;
        }
        *out = databoxNewUnsigned(0);
        return true;
    }

    if (d >= -0x1p63 && d < 0) {
        const int64_t i = (int64_t)d;
        if ((double)i == d) {
            *out = databoxNewSigned(i);
            return true;
        }
        return false;
    }

    if (d > 0 && d < 0x1p64) {
        const uint64_t u = (uint64_t)d;
        if ((double)u == d) {
            *out = databoxNewUnsigned(u);
            return true;
        }
    }
    return false;
}

bool databoxCanonicalNumeral(const databox *in, databoxBig *out) {
    if (!DATABOX_IS_BYTES(in) || in->len == 0) {
        return false;
    }

    const uint8_t *const p = DATABOX_IS_BYTES_EMBED(in)
                                 ? in->data.bytes.embed
                                 : in->data.bytes.custart;
    const size_t len = in->len;

    /* The 64-bit scanner decides every narrow form: integers, floats and
     * doubles. */
    databox narrow = {};
    if (StrScanScanReliable(p, len, &narrow)) {
        memcpy(out, &narrow, sizeof(narrow));
        return true;
    }

    if (len < DATABOX_CANONICAL_WIDE_MIN_LEN ||
        len > DATABOX_CANONICAL_WIDE_MAX_LEN) {
        return false;
    }

    /* The 128-bit scanner adds only the integers beyond 64 bits.  It writes
     * 'out' only for those, so a narrow result here (one the 64-bit scanner
     * just rejected) leaves 'out' untouched and the string stays bytes. */
    databox small;
    databoxBig *use = NULL;
    if (!StrScanScanReliableConvert128(p, len, &small, out, &use) ||
        use != out) {
        return false;
    }

    /* The scanner set the type, the payload and 'big'; the rest of the
     * metadata may be whatever the caller's scratch held. */
    out->allocated = false;
    out->created = false;
    out->unused = 0;
    out->len = 0;
    return true;
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "flex.h"
#include "strDoubleFormat.h"
#include <stdio.h> /* printf */

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);
defcleanerName(databoxBig *, zfree, Big);
defcleaner(flex *, flexFree);
defcleaner(mdsc *, mdscfree);

const char *databoxReprStr(const databox *const box) {
    thread_local static char buf[256];
    thread_local static char bufBig[COUNT_ARRAY(buf)];
    static constexpr size_t bufLen = COUNT_ARRAY(buf);

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
        /* A deferred wide box has a type and no payload to print. */
        if (databoxIsWideDeferred(box)) {
            snprintf(buf, bufLen, "{SIGNED128: deferred}");
            break;
        }

        const int written =
            StrInt128ToBuf(bufBig, sizeof(bufBig), *box->data.i128);
        snprintf(buf, bufLen, "{SIGNED128: %.*s}", written, bufBig);
        break;
    }
    case DATABOX_UNSIGNED_128: {
        if (databoxIsWideDeferred(box)) {
            snprintf(buf, bufLen, "{UNSIGNED128: deferred}");
            break;
        }

        const int written =
            StrUInt128ToBuf(bufBig, sizeof(bufBig), *box->data.u128);
        snprintf(buf, bufLen, "{UNSIGNED128: %.*s}", written, bufBig);
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

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run databoxCompare(a, b) in a child and report whether the child
 * aborted.  The child turns SIGABRT into a distinct exit status, which
 * keeps the platform crash reporter out of it, and sends its stderr to
 * /dev/null so the expected message does not clutter the test log. */
#define DATABOX_TEST_ABORT_STATUS 86
[[noreturn]] static void databoxTestAbortHandler_(int sig) {
    (void)sig;
    _exit(DATABOX_TEST_ABORT_STATUS);
}

static bool databoxTestCompareAborts_(const databox *a, const databox *b) {
    fflush(stdout);
    fflush(stderr);
    const pid_t kid = fork();
    if (kid < 0) {
        return false;
    }

    if (kid == 0) {
        (void)signal(SIGABRT, databoxTestAbortHandler_);
        if (!freopen("/dev/null", "w", stderr)) {
            _exit(2);
        }

        (void)databoxCompare(a, b);
        _exit(0);
    }

    int status = 0;
    if (waitpid(kid, &status, 0) != kid) {
        return false;
    }

    return WIFEXITED(status) &&
           WEXITSTATUS(status) == DATABOX_TEST_ABORT_STATUS;
}

/* Canonical-numeral tests.  The integer oracle below shares no code with
 * the scanners: it checks the syntax itself and classifies the exact
 * magnitude.  Reals have no independent oracle, so the 64-bit scanner
 * decides them and every accepted real must re-print to exactly its input
 * bytes, which is the scanner's contract. */
typedef struct dbCanonTally_ {
    uint32_t strings;
    uint32_t narrow;
    uint32_t wide;
    uint32_t reals;
    uint32_t bytes;
} dbCanonTally_;

/* Decimal spelling of 'magnitude', after a '-' when 'negative'.  'buf'
 * holds at least 42 bytes.  Returns the length. */
static size_t dbCanonSpell_(char *buf, __uint128_t magnitude, bool negative) {
    char reversed[40];
    size_t digits = 0;
    do {
        reversed[digits++] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    } while (magnitude);

    size_t at = 0;
    if (negative) {
        buf[at++] = '-';
    }

    while (digits) {
        buf[at++] = reversed[--digits];
    }

    buf[at] = '\0';
    return at;
}

/* Sets *shaped when 's' is an optional '-' followed by one or more decimal
 * digits.  Returns true, with *expect holding the canonical box, when it is
 * also a canonical integer numeral: no leading zero, not "-0", and within
 * INT128_MIN..UINT128_MAX. */
static bool dbCanonIntegerOracle_(const uint8_t *s, size_t len, bool *shaped,
                                  databoxBig *expect) {
    *shaped = false;
    const bool negative = len > 0 && s[0] == '-';
    const size_t start = negative ? 1 : 0;
    if (len == start) {
        return false;
    }

    for (size_t i = start; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }

    *shaped = true;
    if (s[start] == '0' && (negative || len - start > 1)) {
        return false;
    }

    __uint128_t magnitude = 0;
    for (size_t i = start; i < len; i++) {
        if (ckd_mul(&magnitude, magnitude, (__uint128_t)10) ||
            ckd_add(&magnitude, magnitude, (__uint128_t)(s[i] - '0'))) {
            return false;
        }
    }

    memset(expect, 0, sizeof(*expect));
    if (negative) {
        if (magnitude <= ((__uint128_t)1 << 63)) {
            expect->type = DATABOX_SIGNED_64;
            expect->data.u = 0 - (uint64_t)magnitude;
        } else if (magnitude <= ((__uint128_t)1 << 127)) {
            DATABOX_BIG_SIGNED_128(expect, (__int128_t)(0 - magnitude));
        } else {
            return false;
        }
    } else if (magnitude <= INT64_MAX) {
        expect->type = DATABOX_SIGNED_64;
        expect->data.i = (int64_t)magnitude;
    } else if (magnitude <= UINT64_MAX) {
        expect->type = DATABOX_UNSIGNED_64;
        expect->data.u = (uint64_t)magnitude;
    } else {
        DATABOX_BIG_UNSIGNED_128(expect, magnitude);
    }

    return true;
}

/* Same type and the same value bits. */
static bool dbCanonSame_(const databox *a, const databox *b) {
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
        return a->data.u == b->data.u;
    case DATABOX_FLOAT_32:
        return memcmp(&a->data.f32, &b->data.f32, sizeof(a->data.f32)) == 0;
    case DATABOX_DOUBLE_64:
        return memcmp(&a->data.d64, &b->data.d64, sizeof(a->data.d64)) == 0;
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        return *a->data.u128 == *b->data.u128;
    default:
        return false;
    }
}

/* Check databoxCanonicalNumeral and databoxCanonical on one string, as a
 * BYTES box and (when it fits) a BYTES_EMBED box.  'mustStayBytes' marks a
 * near-numeral that no rule may accept. */
static int dbCanonCheck_(const void *s_, size_t len, bool mustStayBytes,
                         dbCanonTally_ *tally) {
    const uint8_t *const s = s_;
    const int shown = (int)len;
    int err = 0;
    tally->strings++;

    bool shaped = false;
    databoxBig expect = {};
    bool numeral = dbCanonIntegerOracle_(s, len, &shaped, &expect);
    if (shaped) {
        /* Each scanner must agree with the oracle over its own range. */
        databox narrow = {};
        const bool narrowOk = StrScanScanReliable(s, len, &narrow);
        const bool oracleNarrow =
            numeral && (expect.type == DATABOX_SIGNED_64 ||
                        expect.type == DATABOX_UNSIGNED_64);
        if (narrowOk != oracleNarrow ||
            (narrowOk && !dbCanonSame_(&narrow, (const databox *)&expect))) {
            ERR("StrScanScanReliable disagrees with the oracle on '%.*s'",
                shown, s);
        }

        if (len >= 20 && len <= 40) {
            databox small = {};
            databoxBig big = {};
            databoxBig *use = NULL;
            const bool wideOk =
                StrScanScanReliableConvert128(s, len, &small, &big, &use);
            const databox *const scanned =
                use == &big ? (const databox *)&big : &small;
            if (wideOk != numeral ||
                (wideOk && !dbCanonSame_(scanned, (const databox *)&expect))) {
                ERR("StrScanScanReliableConvert128 disagrees with the oracle "
                    "on '%.*s'",
                    shown, s);
            }
        }
    } else {
        databox real = {};
        numeral = StrScanScanReliable(s, len, &real);
        if (numeral) {
            const double v =
                real.type == DATABOX_FLOAT_32 ? real.data.f32 : real.data.d64;
            uint8_t printed[64];
            const size_t printedLen =
                StrDoubleFormatToBufNice(printed, sizeof(printed), v);
            if (!DATABOX_IS_FLOAT(&real)) {
                ERR("'%.*s' scanned as a non-real type %d", shown, s,
                    (int)real.type);
            } else if (printedLen != len || memcmp(printed, s, len) != 0) {
                ERR("real '%.*s' re-prints as '%.*s'", shown, s,
                    (int)printedLen, printed);
            } else if ((real.type == DATABOX_FLOAT_32) !=
                       ((double)(float)v == v)) {
                ERR("real '%.*s' has the wrong width", shown, s);
            }

            memset(&expect, 0, sizeof(expect));
            memcpy(&expect, &real, sizeof(real));
        }
    }

    if (mustStayBytes && numeral) {
        ERR("near-numeral '%.*s' scans as a number", shown, s);
    }

    if (!numeral) {
        tally->bytes++;
    } else if (DATABOX_IS_FLOAT((const databox *)&expect)) {
        tally->reals++;
    } else if (expect.big) {
        tally->wide++;
    } else {
        tally->narrow++;
    }

    for (int embed = 0; embed < 2; embed++) {
        databox in = {};
        if (embed) {
            if (len > sizeof(in.data.bytes.embed)) {
                break;
            }

            in.type = DATABOX_BYTES_EMBED;
            memcpy(in.data.bytes.embed, s, len);
        } else {
            in.type = DATABOX_BYTES;
            in.data.bytes.custart = s;
        }
        in.len = len;

        /* A poisoned 'out': a rejection must leave it alone, and an
         * acceptance must clear everything the result does not use. */
        databoxBig out;
        databoxBig poison;
        memset(&poison, 0xA5, sizeof(poison));
        memcpy(&out, &poison, sizeof(out));
        const bool got = databoxCanonicalNumeral(&in, &out);
        if (got != numeral) {
            ERR("databoxCanonicalNumeral('%.*s', embed %d) returned %d", shown,
                s, embed, (int)got);
        } else if (!got) {
            if (memcmp(&out, &poison, sizeof(out)) != 0) {
                ERR("rejected '%.*s' but wrote 'out'", shown, s);
            }
        } else {
            const bool wide = out.type == DATABOX_SIGNED_128 ||
                              out.type == DATABOX_UNSIGNED_128;
            if (!dbCanonSame_((const databox *)&out,
                              (const databox *)&expect)) {
                ERR("'%.*s' canonicalized to %s", shown, s,
                    databoxReprStr((const databox *)&out));
            } else if (out.allocated || out.created || out.unused || out.len ||
                       out.big != wide ||
                       (wide && out.data.u128 != (__uint128_t *)out.extra)) {
                ERR("'%.*s' canonicalized with stray metadata", shown, s);
            }
        }

        /* The fast reject is exact: the wrapper hands back the scratch
         * exactly when the primitive accepts, and never writes it
         * otherwise. */
        databoxBig scratch;
        memcpy(&scratch, &poison, sizeof(scratch));
        const databox *const use = databoxCanonical(&in, &scratch);
        if (use != &in && use != (const databox *)&scratch) {
            ERR("databoxCanonical('%.*s') returned a foreign box", shown, s);
        } else if ((use == (const databox *)&scratch) != numeral) {
            ERR("databoxCanonical('%.*s', embed %d) disagrees with the "
                "primitive",
                shown, s, embed);
        } else if (use == &in &&
                   memcmp(&scratch, &poison, sizeof(scratch)) != 0) {
            ERR("databoxCanonical('%.*s') wrote an unused scratch", shown, s);
        }
    }

    return err;
}

/* Every near-numeral variant of the numeral 'n', each with one defect, plus
 * real-shaped variants the scanner may accept. */
static int dbCanonMutations_(const char *n, dbCanonTally_ *tally) {
    int err = 0;
    const size_t len = strlen(n);
    const bool negative = n[0] == '-';
    const char *const sign = negative ? "-" : "";
    const char *const digits = n + negative;
    const int digitsLen = (int)strlen(digits);
    char m[128];

#define DB_CANON_MUTATE(mustStayBytes, ...)                                    \
    do {                                                                       \
        const int mlen_ = snprintf(m, sizeof(m), __VA_ARGS__);                 \
        err += dbCanonCheck_(m, (size_t)mlen_, (mustStayBytes), tally);        \
    } while (0)

    /* Syntax defects: always bytes. */
    DB_CANON_MUTATE(true, "%s0%s", sign, digits);  /* leading zero */
    DB_CANON_MUTATE(true, "%s00%s", sign, digits); /* leading zeros */
    DB_CANON_MUTATE(true, "+%s", n);               /* explicit plus */
    DB_CANON_MUTATE(true, " %s", n);               /* leading blank */
    DB_CANON_MUTATE(true, "%s ", n);               /* trailing blank */
    DB_CANON_MUTATE(true, "\t%s", n);              /* leading tab */
    DB_CANON_MUTATE(true, "%s\n", n);              /* trailing newline */
    DB_CANON_MUTATE(true, "x%s", n);               /* leading letter */
    DB_CANON_MUTATE(true, "%sx", n);               /* trailing letter */
    DB_CANON_MUTATE(true, "%se3", n);              /* exponent */
    DB_CANON_MUTATE(true, "%sE-3", n);             /* exponent */
    DB_CANON_MUTATE(true, "%s.", n);               /* trailing dot */
    DB_CANON_MUTATE(true, "%s.50", n);             /* trailing fraction 0 */
    DB_CANON_MUTATE(true, "%s0x%s", sign, digits); /* hex prefix */
    DB_CANON_MUTATE(true, "%s,000", n);            /* digit grouping */
    DB_CANON_MUTATE(true, "%s-", n);               /* trailing minus */
    DB_CANON_MUTATE(true, "--%s", digits);         /* double minus */
    if (digitsLen >= 2) {
        /* a minus inside the digits */
        DB_CANON_MUTATE(true, "%s%.1s-%s", sign, digits, digits + 1);
    }

    /* Bytes the formatter cannot express: an embedded NUL and a high byte
     * after the digits. */
    memcpy(m, n, len);
    m[len] = '\0';
    m[len + 1] = '1';
    err += dbCanonCheck_(m, len + 2, true, tally);
    m[len] = (char)0xB5;
    err += dbCanonCheck_(m, len + 1, true, tally);

    /* Real-shaped variants: the scanner decides, and the re-print rule
     * must hold for whatever it accepts. */
    DB_CANON_MUTATE(false, "%s.0", n);
    DB_CANON_MUTATE(false, "%s.5", n);
    DB_CANON_MUTATE(false, "%s0.%s", sign, digits);
    if (digitsLen >= 2) {
        DB_CANON_MUTATE(false, "%s%.1s.%s", sign, digits, digits + 1);
    }
#undef DB_CANON_MUTATE

    return err;
}

/* xorshift64: a deterministic generator for the real-shaped corpus. */
static uint64_t dbCanonNext_(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* Exact-value oracle for the numeric order, built from bit patterns rather
 * than floating-point arithmetic.  A finite value is (-1)^negative *
 * mantissa * 2^exponent, zero is never negative, and the ranks place -inf
 * below every finite value, then +inf, then NaN. */
typedef struct dbOrderValue_ {
    int rank; /* 0 -inf, 1 finite, 2 +inf, 3 NaN */
    bool negative;
    __uint128_t mantissa;
    int exponent;
} dbOrderValue_;

static dbOrderValue_ dbOrderOfDouble_(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    const bool sign = bits >> 63;
    const uint32_t biased = (uint32_t)((bits >> 52) & 0x7FF);
    const uint64_t fraction = bits & ((UINT64_C(1) << 52) - 1);
    dbOrderValue_ v = {.rank = 1};
    if (biased == 0x7FF) {
        v.rank = fraction ? 3 : (sign ? 0 : 2);
        return v;
    }

    if (biased == 0) {
        v.mantissa = fraction;
        v.exponent = -1074;
    } else {
        v.mantissa = fraction | (UINT64_C(1) << 52);
        v.exponent = (int)biased - 1075;
    }

    v.negative = sign && v.mantissa != 0;
    return v;
}

static dbOrderValue_ dbOrderOf_(const databox *box) {
    dbOrderValue_ v = {.rank = 1};
    switch (box->type) {
    case DATABOX_FLOAT_32:
        return dbOrderOfDouble_((double)box->data.f32);
    case DATABOX_DOUBLE_64:
        return dbOrderOfDouble_(box->data.d64);
    case DATABOX_SIGNED_64:
        v.negative = box->data.i < 0;
        v.mantissa = v.negative ? (__uint128_t)(-(box->data.i + 1)) + 1
                                : (__uint128_t)box->data.i;
        return v;
    case DATABOX_SIGNED_128: {
        const __int128_t i = *box->data.i128;
        v.negative = i < 0;
        v.mantissa = v.negative ? (__uint128_t)(-(i + 1)) + 1 : (__uint128_t)i;
        return v;
    }
    case DATABOX_UNSIGNED_128:
        v.mantissa = *box->data.u128;
        return v;
    default: /* UNSIGNED_64, PTR, REFERENCE */
        v.mantissa = box->data.u;
        return v;
    }
}

static int dbOrderBits_(__uint128_t m) {
    int bits = 0;
    while (m) {
        bits++;
        m >>= 1;
    }

    return bits;
}

static int dbOrderCompare_(const dbOrderValue_ *a, const dbOrderValue_ *b) {
    if (a->rank != b->rank) {
        return a->rank < b->rank ? -1 : 1;
    }

    if (a->rank != 1) {
        return 0; /* the same infinity, or two NaNs */
    }

    if (a->negative != b->negative) {
        return a->negative ? -1 : 1;
    }

    /* Compare magnitudes: zero first, then by binary length, then aligned. */
    int cmp;
    if (a->mantissa == 0 || b->mantissa == 0) {
        cmp = (a->mantissa != 0) - (b->mantissa != 0);
    } else {
        const int aLen = dbOrderBits_(a->mantissa) + a->exponent;
        const int bLen = dbOrderBits_(b->mantissa) + b->exponent;
        if (aLen != bLen) {
            cmp = aLen < bLen ? -1 : 1;
        } else {
            /* Equal lengths: shift the shorter mantissa up to the longer's
             * width; neither then exceeds 128 bits. */
            __uint128_t am = a->mantissa;
            __uint128_t bm = b->mantissa;
            if (a->exponent > b->exponent) {
                am <<= (a->exponent - b->exponent);
            } else {
                bm <<= (b->exponent - a->exponent);
            }
            cmp = am < bm ? -1 : am > bm ? 1 : 0;
        }
    }

    return a->negative ? -cmp : cmp;
}

/* A free hook that records what it is asked to free and frees nothing,
 * so a test can hand databoxFreeData a pointer it does not own and see
 * whether the free is attempted */
static const void *dbFreeRecorded_;
static size_t dbFreeCalls_;
static void dbFreeRecord_(void *ptr) {
    dbFreeRecorded_ = ptr;
    dbFreeCalls_ += ptr != NULL;
}

int databoxTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    assert(sizeof(databox) == 16);

    TEST("an integral real is the integer it equals, at every boundary") {
        /* Whole numbers in [-2^63, 2^64) become SIGNED (negative) or
         * UNSIGNED; -0.0, fractions, infinities, NaN and values outside
         * stay reals; floats widen first */
        databox f7 = {};
        DATABOX_SET_FLOAT(&f7, 7.0f);
        databox fHalf = {};
        DATABOX_SET_FLOAT(&fHalf, 7.5f);
        static const struct {
            double d;
            int kind; /* 0 stays real, 1 unsigned, 2 signed */
            uint64_t bits;
        } cases[] = {
            {5.0, 1, 5},
            {0.0, 1, 0},
            {-0.0, 0, 0},
            {-3.0, 2, (uint64_t)-3},
            {0.5, 0, 0},
            {-1e-300, 0, 0},
            {0x1p63, 1, UINT64_C(1) << 63},
            {0x1p64 - 2048, 1, UINT64_MAX - 2047},
            {0x1p64, 0, 0},
            {-0x1p63, 2, UINT64_C(1) << 63},
            {-0x1p63 - 2048, 0, 0},
            {INFINITY, 0, 0},
            {-INFINITY, 0, 0},
            {NAN, 0, 0},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            const databox in = DATABOX_DOUBLE(cases[i].d);
            databox out = databoxNewSigned(-77);
            const bool whole = databoxIntegralRealAsInteger(&in, &out);
            const bool ok = cases[i].kind == 0
                                ? !whole && out.data.i == -77
                                : whole &&
                                      out.type == (cases[i].kind == 1
                                                       ? DATABOX_UNSIGNED_64
                                                       : DATABOX_SIGNED_64) &&
                                      out.data.u == cases[i].bits;
            if (!ok) {
                ERR("case %zu (%g): whole %d, type %d", i, cases[i].d, whole,
                    (int)out.type);
            }
        }
        databox out = {};
        const databox text = databoxNewBytesString("5");
        if (!databoxIntegralRealAsInteger(&f7, &out) || out.data.u != 7 ||
            databoxIntegralRealAsInteger(&fHalf, &out) ||
            databoxIntegralRealAsInteger(&text, &out)) {
            ERR("floats or a non-real box were handled wrongly");
        }

        /* The minimal-storage canonical form applies it to real boxes;
         * the numeral "22.0" stays the real 22.0, keeping its spelling */
        databoxBig scratch;
        const databox five = DATABOX_DOUBLE(5.0);
        const databox s22 = databoxNewBytesString("22.0");
        const databox *c5 = databoxCanonical(&five, &scratch);
        const bool c5ok = c5->type == DATABOX_UNSIGNED_64 && c5->data.u == 5;
        const databox *c22 = databoxCanonical(&s22, &scratch);
        if (!c5ok || !DATABOX_IS_FLOAT(c22)) {
            ERR("the canonical form did not make a whole real an integer, "
                "or changed a numeral's spelling");
        }
    }

    TEST("retain cache: borrow vs own vs embed ownership") {
        static constexpr char payload[] =
            "this payload is comfortably longer than the embed threshold";
        const size_t payloadLen = sizeof(payload) - 1;

        /* Slot path: box BORROWS cache memory (allocated stays false),
         * so FreeData must not free the cache's buffer. */
        databoxRetainCache cache = {};
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
        databoxRetainCache empty = {};
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

        /* Embedded bytes reference nothing outside the box: untouched */
        databox embedded = databoxNewBytesAllowEmbed("inline", 6);
        const databox embeddedBefore = embedded;
        if (embedded.type != DATABOX_BYTES_EMBED ||
            databoxRetainBytesSelf(&embedded, &cache) != -2 ||
            memcmp(&embedded, &embeddedBefore, sizeof(embedded)) != 0) {
            ERR("an embedded box was not left as it was");
        }

        /* Bytes the source never frees are retained as owned BYTES; an
         * mdsc string is retained as a new mdsc; both free cleanly */
        databox never = {.type = DATABOX_BYTES_NEVER_FREE,
                         .data.bytes.start = (uint8_t *)payload,
                         .len = payloadLen};
        cleaner(mdscfree) mdsc *str = mdscnew(payload);
        databox viaMdsc = {
            .type = DATABOX_PTR_MDSC, .data.ptr = str, .len = mdsclen(str)};
        databoxRetainCache none = {};
        const ssize_t neverAt = databoxRetainBytesSelf(&never, &none);
        const ssize_t mdscAt = databoxRetainBytesSelf(&viaMdsc, &none);
        const bool neverOk =
            neverAt == -1 && never.type == DATABOX_BYTES && never.allocated &&
            memcmp(databoxBytes(&never), payload, payloadLen) == 0;
        const bool mdscOk =
            mdscAt == -1 && viaMdsc.type == DATABOX_PTR_MDSC &&
            viaMdsc.data.ptr != str &&
            memcmp(databoxBytes(&viaMdsc), payload, payloadLen) == 0;
        databoxFreeData(&never);
        databoxFreeData(&viaMdsc);
        if (!neverOk || !mdscOk || never.allocated || viaMdsc.allocated) {
            ERR("retain: never-free %d (at %zd), mdsc %d (at %zd)", neverOk,
                neverAt, mdscOk, mdscAt);
        }

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
        int64_t signedVals[] = {INT64_MIN, INT64_MIN + 1, -1'000'000, -1, 0, 1,
                                1'000'000, INT64_MAX - 1, INT64_MAX};
        for (size_t i = 0; i < sizeof(signedVals) / sizeof(signedVals[0]);
             i++) {
            databox a = databoxNewSigned(signedVals[i]);
            if (databoxCompare(&a, &a) != 0) {
                ERR("Reflexivity failed for signed %" PRId64, signedVals[i]);
            }
        }

        /* Test unsigned integers */
        uint64_t unsignedVals[] = {0, 1, 1'000'000, UINT64_MAX - 1, UINT64_MAX};
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
            ERR("Transitivity failed for signed integers");
        }

        /* Test with extreme values */
        a = databoxNewSigned(INT64_MIN);
        b = databoxNewSigned(0);
        c = databoxNewSigned(INT64_MAX);
        if (!(databoxCompare(&a, &b) < 0 && databoxCompare(&b, &c) < 0 &&
              databoxCompare(&a, &c) < 0)) {
            ERR("Transitivity failed at boundaries");
        }

        /* Test with strings */
        a = databoxNewBytesString("aaa");
        b = databoxNewBytesString("bbb");
        c = databoxNewBytesString("ccc");
        if (!(databoxCompare(&a, &b) < 0 && databoxCompare(&b, &c) < 0 &&
              databoxCompare(&a, &c) < 0)) {
            ERR("Transitivity failed for strings");
        }

        /* Test with floats */
        databox f1 = databoxNewReal(-100.0f);
        databox f2 = databoxNewReal(0.0f);
        databox f3 = databoxNewReal(100.0f);
        if (!(databoxCompare(&f1, &f2) < 0 && databoxCompare(&f2, &f3) < 0 &&
              databoxCompare(&f1, &f3) < 0)) {
            ERR("Transitivity failed for floats");
        }
    }

    TEST("signed vs unsigned integer comparison consistency") {
        /* Positive values should compare equal regardless of signed/unsigned */
        databox s = databoxNewSigned(100);
        databox u = databoxNewUnsigned(100);
        if (databoxCompare(&s, &u) != 0) {
            ERR("Same positive value differs between signed/unsigned");
        }

        /* Zero */
        s = databoxNewSigned(0);
        u = databoxNewUnsigned(0);
        if (databoxCompare(&s, &u) != 0) {
            ERR("Zero differs between signed/unsigned");
        }

        /* Negative signed should be less than any unsigned */
        s = databoxNewSigned(-1);
        u = databoxNewUnsigned(0);
        if (databoxCompare(&s, &u) >= 0) {
            ERR("Negative signed should be less than unsigned 0");
        }

        /* Large unsigned values */
        s = databoxNewSigned(INT64_MAX);
        u = databoxNewUnsigned((uint64_t)INT64_MAX + 1);
        if (databoxCompare(&s, &u) >= 0) {
            ERR("INT64_MAX should be less than INT64_MAX+1 unsigned");
        }
    }

    TEST("integer vs float comparison consistency") {
        /* Equal values */
        databox ibox = databoxNewSigned(100);
        databox fbox = databoxNewReal(100.0f);
        if (databoxCompare(&ibox, &fbox) != 0) {
            ERR("100 int should equal 100.0 float");
        }

        /* Integer less than float */
        ibox = databoxNewSigned(99);
        fbox = databoxNewReal(99.5f);
        if (databoxCompare(&ibox, &fbox) >= 0) {
            ERR("99 int should be less than 99.5 float");
        }

        /* Integer greater than float */
        ibox = databoxNewSigned(100);
        fbox = databoxNewReal(99.5f);
        if (databoxCompare(&ibox, &fbox) <= 0) {
            ERR("100 int should be greater than 99.5 float");
        }

        /* Negative values */
        ibox = databoxNewSigned(-50);
        fbox = databoxNewReal(-50.0);
        if (databoxCompare(&ibox, &fbox) != 0) {
            ERR("-50 int should equal -50.0 double");
        }
    }

    TEST("float vs double comparison consistency") {
        databox f = databoxNewReal(1.5f);
        databox d = databoxNewReal(1.5);
        if (databoxCompare(&f, &d) != 0) {
            ERR("1.5 float should equal 1.5 double");
        }

        f = databoxNewReal(1.0f);
        d = databoxNewReal(1.1);
        if (databoxCompare(&f, &d) >= 0) {
            ERR("1.0 float should be less than 1.1 double");
        }

        f = databoxNewReal(-100.5f);
        d = databoxNewReal(-100.5);
        if (databoxCompare(&f, &d) != 0) {
            ERR("-100.5 float should equal -100.5 double");
        }
    }

    TEST("string comparison with numeric prefixes (natural sort)") {
        /* databox uses natural sort where numeric strings compare by value */
        databox s1 = databoxNewBytesString("10");
        databox s2 = databoxNewBytesString("2");
        /* "10" > "2" in natural sort because 10 > 2 numerically */
        if (databoxCompare(&s1, &s2) <= 0) {
            ERR("'10' should be greater than '2' in natural sort");
        }

        s1 = databoxNewBytesString("100");
        s2 = databoxNewBytesString("99");
        /* "100" > "99" in natural sort because 100 > 99 numerically */
        if (databoxCompare(&s1, &s2) <= 0) {
            ERR("'100' should be greater than '99' in natural sort");
        }

        /* Same prefix, different length (non-numeric) */
        s1 = databoxNewBytesString("abc");
        s2 = databoxNewBytesString("abcd");
        if (databoxCompare(&s1, &s2) >= 0) {
            ERR("'abc' should be less than 'abcd'");
        }

        /* Verify natural sort with mixed content */
        s1 = databoxNewBytesString("file2.txt");
        s2 = databoxNewBytesString("file10.txt");
        /* "file2.txt" < "file10.txt" because 2 < 10 */
        if (databoxCompare(&s1, &s2) >= 0) {
            ERR("'file2.txt' should be less than 'file10.txt' in natural sort");
        }
    }

    TEST("databoxEqual is IEEE value equality; compare is identity") {
        const databox nanD = databoxNewReal(NAN);
        const databox nanNegD = databoxNewReal(-NAN);
        const databox nanF = {.type = DATABOX_FLOAT_32, .data.f32 = NAN};
        const databox inf = databoxNewReal(INFINITY);
        const databox zero = databoxNewReal(0.0);
        const databox negZero = databoxNewReal(-0.0);
        const databox intZero = databoxNewSigned(0);
        const databox one = databoxNewSigned(1);
        const databox *const nans[] = {&nanD, &nanNegD, &nanF};

        for (size_t i = 0; i < COUNT_ARRAY(nans); i++) {
            /* NaN equals nothing, not even itself... */
            if (databoxEqual(nans[i], nans[i])) {
                ERR("NaN %zu equals itself", i);
            }
            for (size_t j = 0; j < COUNT_ARRAY(nans); j++) {
                if (databoxEqual(nans[i], nans[j])) {
                    ERR("NaN %zu equals NaN %zu", i, j);
                }
                /* ...but every NaN is one identity position. */
                if (databoxCompare(nans[i], nans[j]) != 0) {
                    ERR("NaN %zu and NaN %zu differ in the identity order", i,
                        j);
                }
            }
            if (databoxEqual(nans[i], &one) || databoxEqual(&one, nans[i])) {
                ERR("NaN %zu equals 1", i);
            }
            /* NaN sorts above +inf, on every run. */
            if (databoxCompare(nans[i], &inf) <= 0 ||
                databoxCompare(&inf, nans[i]) >= 0) {
                ERR("NaN %zu does not sort above +inf", i);
            }
        }

        if (!databoxEqual(&zero, &negZero) || !databoxEqual(&negZero, &zero) ||
            !databoxEqual(&negZero, &intZero)) {
            ERR("-0.0, +0.0 and 0 are not equal");
        }
        if (!databoxEqual(&one, &one) || databoxEqual(&one, &intZero)) {
            ERR("integer equality is wrong");
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
                        ERR("natural-sort comparator is not transitive");
                    }
                    if (ij > 0 && jk > 0 && ik <= 0) {
                        printf("transitivity broken: %s > %s and %s > %s "
                               "but %s vs %s = %d\n",
                               cases[i], cases[j], cases[j], cases[k], cases[i],
                               cases[k], ik);
                        ERR("natural-sort comparator is not transitive");
                    }
                    if (ij == 0 && jk == 0 && ik != 0) {
                        ERR("natural-sort equality is not transitive");
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
                    ERR("natural-sort antisymmetry violated (zero vs nonzero)");
                }
                if (ab > 0 && ba >= 0) {
                    ERR("natural-sort antisymmetry violated (both positive)");
                }
                if (ab < 0 && ba <= 0) {
                    ERR("natural-sort antisymmetry violated (both negative)");
                }
            }
        }
        /* Specific orderings the natural sort must preserve. */
        databox a = databoxNewBytesString("g_2_39");
        databox b = databoxNewBytesString("g_10_0");
        databox c = databoxNewBytesString("g_1_6");
        if (!(databoxCompare(&c, &a) < 0)) {
            ERR("'g_1_6' must be less than 'g_2_39' (1 < 2)");
        }
        if (!(databoxCompare(&a, &b) < 0)) {
            ERR("'g_2_39' must be less than 'g_10_0' (2 < 10)");
        }
        if (!(databoxCompare(&c, &b) < 0)) {
            ERR("'g_1_6' must be less than 'g_10_0' (1 < 10)");
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
                ERR("leading-zero compare asymmetric");
            }
        } else if ((ab > 0 && ba < 0) || (ab < 0 && ba > 0)) {
            /* Good: opposite signs. */
        } else {
            ERR("leading-zero compare has same sign in both directions");
        }
        databox c = databoxNewBytesString("0001");
        const int ac = databoxCompare(&a, &c);
        const int ca = databoxCompare(&c, &a);
        if ((ac > 0 && ca >= 0) || (ac < 0 && ca <= 0)) {
            ERR("zero-run length compare violates antisymmetry");
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
                ERR("natural sort produces non-monotonic ordering");
            }
        }
    }

    TEST("empty string comparisons") {
        databox empty = databoxNewBytesString("");
        databox nonEmpty = databoxNewBytesString("a");
        if (databoxCompare(&empty, &nonEmpty) >= 0) {
            ERR("Empty string should be less than any non-empty string");
        }

        databox empty2 = databoxNewBytesString("");
        if (databoxCompare(&empty, &empty2) != 0) {
            ERR("Empty strings should be equal");
        }
    }

    TEST("embedded vs non-embedded bytes comparison") {
        /* Short string that can be embedded */
        const char *shortStr = "hello";
        databox embed = databoxNewBytesAllowEmbed(shortStr, strlen(shortStr));
        databox noEmbed = databoxNewBytes(shortStr, strlen(shortStr));
        if (databoxCompare(&embed, &noEmbed) != 0) {
            ERR("Embedded and non-embedded same string should be equal");
        }

        /* Long string that won't be embedded */
        const char *longStr = "this is a longer string that cannot be embedded";
        databox long1 = databoxNewBytesString(longStr);
        databox long2 = databoxNewBytesAllowEmbed(longStr, strlen(longStr));
        if (databoxCompare(&long1, &long2) != 0) {
            ERR("Long strings should compare equal regardless of type");
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
        int64_t testVals[] = {INT64_MIN,
                              INT64_MIN / 2,
                              -1'000'000'000LL,
                              -1'000'000LL,
                              -1000LL,
                              -1LL,
                              0LL,
                              1LL,
                              1000LL,
                              1'000'000LL,
                              1'000'000'000LL,
                              INT64_MAX / 2,
                              INT64_MAX};
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
            ERR("void type incorrect");
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
            ERR("True type incorrect");
        }
        if (falseBox.type != DATABOX_FALSE) {
            ERR("False type incorrect");
        }

        /* Test NULL */
        databox nullBox = databoxNull();
        if (nullBox.type != DATABOX_NULL) {
            ERR("Null type incorrect");
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

    TEST("databoxBig: the 128-bit store is 16-byte aligned in any layout") {
        /* The 8-byte member ahead of the box puts it at 8 mod 16 whenever
         * databoxBig itself is only 8-aligned. */
        struct {
            alignas(16) uint64_t pad;
            databoxBig b;
        } padded;
        databoxBig arr[3];
        cleanerName(zfree, Big) databoxBig *heap = zcalloc(3, sizeof(*heap));
        databoxBig *const where[] = {&padded.b, &arr[0],  &arr[1], &arr[2],
                                     &heap[0],  &heap[1], &heap[2]};
        if (alignof(databoxBig) != 16 || sizeof(databoxBig) != 32 ||
            offsetof(databoxBig, extra) != 16) {
            ERR("databoxBig layout is not 32 bytes with a 16-aligned extra");
        }

        /* Decode a 128-bit entry into each placement: the store through
         * data.u128 is a full __uint128_t write. */
        const __uint128_t wide = UINT128_MAX - 12345;
        databoxBig in;
        DATABOX_BIG_UNSIGNED_128(&in, wide);
        cleaner(flexFree) flex *f = flexNewFromDataboxBig(&in);
        for (size_t i = 0; i < COUNT_ARRAY(where); i++) {
            databoxBig *const b = where[i];
            if (((uintptr_t)b->extra % 16) != 0) {
                ERR("databoxBig.extra at placement %zu is not 16-aligned", i);
                continue;
            }

            DATABOX_BIG_INIT(b);
            flexGetByType(flexHead(f), (databox *)b);
            if (b->type != DATABOX_UNSIGNED_128 || *b->data.u128 != wide) {
                ERR("128-bit decode at placement %zu read back wrong", i);
            }
        }
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
            ERR("PTR vs PTR must order by pointer value");
        }
        if (databoxCompare(&pLo, &pLo) != 0) {
            ERR("PTR must compare equal to itself");
        }
        if (databoxCompare(&pLo, &uSame) != 0 ||
            databoxCompare(&uSame, &pLo) != 0) {
            ERR("PTR must equal the UNSIGNED box flex decode returns");
        }
        if (databoxCompare(&pLo, &sNeg) <= 0) {
            ERR("PTR (as unsigned) must order above negative signed");
        }
    }

    TEST("databoxCompare: every numeric type pair against exact values") {
        /* Values held as twice themselves (halves stay exact), each in
         * every numeric type that represents it exactly: every pair of
         * boxes compares with the sign of their exact difference */
        static const __int128 twice[] = {-((__int128)1 << 65),
                                         -10,
                                         -5,
                                         0,
                                         5,
                                         6,
                                         (__int128)1 << 41,
                                         ((__int128)1 << 41) + 1,
                                         (__int128)1 << 63,
                                         (__int128)1 << 66,
                                         ((__int128)UINT64_MAX) * 2};
        static const databoxType types[] = {
            DATABOX_SIGNED_64, DATABOX_UNSIGNED_64, DATABOX_FLOAT_32,
            DATABOX_DOUBLE_64, DATABOX_SIGNED_128,  DATABOX_UNSIGNED_128};
        enum { NV = sizeof(twice) / sizeof(*twice) };
        enum { NT = sizeof(types) / sizeof(*types) };
        databoxBig boxes[NV * NT];
        __int128 value2[NV * NT];
        size_t n = 0;
        for (size_t v = 0; v < NV; v++) {
            const __int128 t = twice[v];
            const bool integral = t % 2 == 0;
            const __int128 whole = t / 2;
            for (size_t k = 0; k < NT; k++) {
                databoxBig *b = &boxes[n];
                memset(b, 0, sizeof(*b));
                bool ok = false;
                switch (types[k]) {
                case DATABOX_SIGNED_64:
                    ok = integral && whole >= INT64_MIN && whole <= INT64_MAX;
                    *b = DATABOX_BIG_SIGNED((int64_t)whole);
                    break;
                case DATABOX_UNSIGNED_64:
                    ok = integral && whole >= 0 && whole <= UINT64_MAX;
                    *b = DATABOX_BIG_UNSIGNED((uint64_t)whole);
                    break;
                case DATABOX_FLOAT_32: {
                    const float f = (float)((double)t / 2.0);
                    ok = (__int128)((double)f * 2.0) == t;
                    b->type = DATABOX_FLOAT_32;
                    b->data.f32 = f;
                    break;
                }
                case DATABOX_DOUBLE_64: {
                    const double d = (double)t / 2.0;
                    ok = (__int128)(d * 2.0) == t;
                    b->type = DATABOX_DOUBLE_64;
                    b->data.d64 = d;
                    break;
                }
                case DATABOX_SIGNED_128:
                    ok = integral;
                    DATABOX_BIG_SIGNED_128(b, whole);
                    break;
                case DATABOX_UNSIGNED_128:
                    ok = integral && whole >= 0;
                    DATABOX_BIG_UNSIGNED_128(b, (__uint128_t)whole);
                    break;
                default:
                    break;
                }
                if (ok) {
                    value2[n++] = t;
                }
            }
        }
        size_t wrong = 0;
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                const int want = value2[i] < value2[j]   ? -1
                                 : value2[i] > value2[j] ? 1
                                                         : 0;
                const int got = databoxCompare((const databox *)&boxes[i],
                                               (const databox *)&boxes[j]);
                if ((got > 0) - (got < 0) != want) {
                    if (wrong++ < 4) {
                        ERR("types %d vs %d: compare %d, want %d",
                            (int)boxes[i].type, (int)boxes[j].type, got, want);
                    }
                }
            }
        }
        if (wrong) {
            ERR("%zu of %zu numeric comparisons wrong", wrong, n * n);
        }
    }

    TEST("DATABOX_SIZE counts a 128-bit value as 16 bytes") {
        databoxBig wide;
        DATABOX_BIG_SIGNED_128(&wide, -((__int128)1 << 100));
        const databox small = databoxNewSigned(-3);
        const databox text = databoxNewBytes("twelve bytes", 12);
        if (DATABOX_SIZE((const databox *)&wide) != 16 ||
            DATABOX_SIZE(&small) != 8 || DATABOX_SIZE(&text) != 12) {
            ERR("DATABOX_SIZE misjudged a value");
        }
    }

    TEST("databoxGetBytes: every bytes-carrying type, false for the rest") {
        /* BYTES, NEVER_FREE, embedded bytes and an mdsc string give their
         * bytes; booleans, null and a 128-bit integer have no byte form
         * and give false (not a debug assertion) */
        uint8_t text[] = "some bytes to read";
        cleaner(mdscfree) mdsc *str = mdscnew("mdsc bytes");
        databox withBytes[] = {
            {.type = DATABOX_BYTES, .data.bytes.start = text, .len = 18},
            {.type = DATABOX_BYTES_NEVER_FREE,
             .data.bytes.start = text,
             .len = 18},
            databoxNewBytesAllowEmbed("tiny", 4),
            {.type = DATABOX_PTR_MDSC, .data.ptr = str, .len = mdsclen(str)},
        };
        static const char *const want[] = {
            "some bytes to read", "some bytes to read", "tiny", "mdsc bytes"};
        for (size_t i = 0; i < sizeof(withBytes) / sizeof(*withBytes); i++) {
            uint8_t *buf = NULL;
            size_t len = 0;
            size_t size = 0;
            if (!databoxGetBytes(&withBytes[i], &buf, &len) ||
                len != strlen(want[i]) || memcmp(buf, want[i], len) != 0 ||
                !databoxGetSize(&withBytes[i], &size) || size != len) {
                ERR("bytes box %zu: no bytes, the wrong ones, or another size",
                    i);
            }
        }
        databox none[] = {{.type = DATABOX_TRUE},
                          {.type = DATABOX_FALSE},
                          {.type = DATABOX_NULL},
                          {.type = DATABOX_SIGNED_128}};
        for (size_t i = 0; i < sizeof(none) / sizeof(*none); i++) {
            uint8_t *buf = NULL;
            size_t len = 0;
            if (databoxGetBytes(&none[i], &buf, &len)) {
                ERR("type %d gave bytes", (int)none[i].type);
            }
        }
    }

    TEST("databox: owning copies of every byte-carrying type") {
        /* Each source, copied with databoxCopy and made owned in place
         * with databoxAllocateIfNeeded: the copy keeps its type (bytes
         * the source never frees become owned BYTES), holds the same
         * bytes in its own storage, and databoxFreeData releases it */
        uint8_t text20[] = "twenty bytes of text";
        cleaner(mdscfree) mdsc *mds20 = mdscnew("an mdsc of 23 bytes!!!");
        cleaner(mdscfree) mdsc *mds3 = mdscnew("xyz");
        struct {
            databox box;
            databoxType want;
        } cases[] = {
            {{.type = DATABOX_BYTES, .data.bytes.start = text20, .len = 20},
             DATABOX_BYTES},
            {{.type = DATABOX_BYTES_NEVER_FREE,
              .data.bytes.start = text20,
              .len = 20},
             DATABOX_BYTES},
            {{.type = DATABOX_PTR_MDSC,
              .data.ptr = mds20,
              .len = mdsclen(mds20)},
             DATABOX_PTR_MDSC},
            {{.type = DATABOX_PTR_MDSC, .data.ptr = mds3, .len = 3},
             DATABOX_PTR_MDSC},
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
            for (int inPlace = 0; inPlace <= 1; inPlace++) {
                const databox *src = &cases[c].box;
                databox copy = *src;
                if (inPlace) {
                    databoxAllocateIfNeeded(&copy);
                } else {
                    copy = databoxCopy(src);
                }
                const bool own = databoxBytes(&copy) != databoxBytes(src);
                const bool same = databoxLen(&copy) == databoxLen(src) &&
                                  memcmp(databoxBytes(&copy), databoxBytes(src),
                                         databoxLen(src)) == 0;
                const bool typed = copy.type == cases[c].want;
                databoxFreeData(&copy);
                if (!own || !same || !typed || copy.allocated) {
                    ERR("case %zu (%s): own %d same %d type %d (want %d) "
                        "freed %d",
                        c, inPlace ? "in place" : "copy", own, same,
                        (int)copy.type, (int)cases[c].want, !copy.allocated);
                }
            }
        }
    }

    TEST("databox: copy helpers own 96/128-bit payloads") {
        /* Wide oracle values; negatives are SIGNED_128, the rest
         * UNSIGNED_128. */
        const __int128_t sw[] = {
            -((__int128_t)1 << 63) - 1,
            -((__int128_t)1 << 95),
            INT128_MIN,
        };
        const __uint128_t uw[] = {
            (__uint128_t)1 << 64, ((__uint128_t)1 << 95) - 1,
            (__uint128_t)1 << 95, (__uint128_t)1 << 127,
            UINT128_MAX,          (__uint128_t)INT128_MAX,
        };
        const size_t n = COUNT_ARRAY(sw) + COUNT_ARRAY(uw);

        for (size_t i = 0; i < n; i++) {
            const bool neg = i < COUNT_ARRAY(sw);
            const __uint128_t want =
                neg ? (__uint128_t)sw[i] : uw[i - COUNT_ARRAY(sw)];
            const databoxType type =
                neg ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;

            databoxBig src;
            memset(&src, 0, sizeof(src));
            DATABOX_BIG_INIT(&src);
            src.type = type;
            memcpy(src.extra, &want, sizeof(want));

            /* databoxCopy: owned heap payload, independent of 'src'. */
            databox copy = databoxCopy((const databox *)&src);

            /* databoxCopyBytesFromBox into a poisoned destination. */
            databox bytesCopy;
            memset(&bytesCopy, 0xFF, sizeof(bytesCopy));
            databoxCopyBytesFromBox(&bytesCopy, (const databox *)&src);

            /* databoxAllocateIfNeeded on a box borrowing src.extra. */
            databox borrowed;
            memcpy(&borrowed, &src, sizeof(borrowed));
            const bool allocated = databoxAllocateIfNeeded(&borrowed);
            const void *const ownedPtr = borrowed.data.ptr;
            const bool again = databoxAllocateIfNeeded(&borrowed);

            /* Overwrite the source: every copy must keep its value. */
            memset(src.extra, 0xA5, sizeof(src.extra));

            databox *const copies[] = {&copy, &bytesCopy, &borrowed};
            for (size_t c = 0; c < COUNT_ARRAY(copies); c++) {
                const databox *const got = copies[c];
                __uint128_t v = 0;
                if (got->data.ptr) {
                    memcpy(&v, got->data.ptr, sizeof(v));
                }
                if (got->type != type || !got->allocated || !got->big ||
                    !got->data.ptr || got->data.ptr == (void *)src.extra ||
                    v != want) {
                    ERR("wide value %zu copy %zu is not an owned copy", i, c);
                }
            }

            if (!allocated || again || borrowed.data.ptr != ownedPtr) {
                ERR("wide value %zu: AllocateIfNeeded returned %d then %d", i,
                    allocated, again);
            }

            for (size_t c = 0; c < COUNT_ARRAY(copies); c++) {
                databoxFreeData(copies[c]);
                if (copies[c]->type != DATABOX_VOID || copies[c]->data.ptr ||
                    copies[c]->allocated || copies[c]->big) {
                    ERR("wide value %zu copy %zu not released", i, c);
                }
            }
        }

        /* A type-only wide box has no payload and stays type-only. */
        const databox typeOnly = {.type = DATABOX_SIGNED_128};
        databox t = databoxCopy(&typeOnly);
        databox t2 = typeOnly;
        if (t.type != DATABOX_SIGNED_128 || t.data.ptr || t.allocated ||
            t.big || databoxAllocateIfNeeded(&t2) || t2.allocated) {
            ERR("type-only wide box gained a payload");
        }
        databoxFreeData(&t);

        /* A narrow copy of a databoxBig carries no live pointer. */
        databoxBig narrow;
        memset(&narrow, 0, sizeof(narrow));
        DATABOX_BIG_INIT(&narrow);
        narrow.type = DATABOX_SIGNED_64;
        narrow.data.i64 = -42;
        databox narrowCopy = databoxCopy((const databox *)&narrow);
        databox narrowBytes;
        memset(&narrowBytes, 0xFF, sizeof(narrowBytes));
        databoxCopyBytesFromBox(&narrowBytes, (const databox *)&narrow);
        if (narrowCopy.big || narrowCopy.data.i64 != -42 || narrowBytes.big ||
            narrowBytes.data.i64 != -42) {
            ERR("narrow copy of a databoxBig kept big = 1");
        }

        /* A wide value held inline is not freed, even when a poisoned
         * box leaves 'allocated' set. */
        databoxBig inl;
        memset(&inl, 0xFF, sizeof(inl));
        DATABOX_BIG_SIGNED_128(&inl, INT128_MIN + 7);
        databoxFreeData((databox *)&inl);
        if (inl.type != DATABOX_SIGNED_128 ||
            inl.data.i128 != (__int128_t *)inl.extra ||
            *inl.data.i128 != INT128_MIN + 7) {
            ERR("FreeData touched an inline wide payload");
        }
    }

    TEST("databoxBigCopyFrom: re-homes a wide value without allocating") {
        const __uint128_t want = ((__uint128_t)1 << 127) + 99;

        /* From a heap-owned copy: the copy survives freeing the source. */
        databoxBig src;
        DATABOX_BIG_UNSIGNED_128(&src, want);
        databox owned = databoxCopy((const databox *)&src);
        databoxBig dst;
        memset(&dst, 0xFF, sizeof(dst));
        databoxBigCopyFrom(&dst, &owned);
        databoxFreeData(&owned);
        if (dst.type != DATABOX_UNSIGNED_128 || !dst.big || dst.allocated ||
            dst.data.u128 != (__uint128_t *)dst.extra ||
            *dst.data.u128 != want) {
            ERR("re-homed copy of an owned wide box is wrong");
        }

        /* From a box borrowing other storage, aliased in place. */
        __uint128_t outside = want - 5;
        databoxBig self;
        memset(&self, 0xFF, sizeof(self));
        self.type = DATABOX_UNSIGNED_128;
        self.allocated = false;
        self.big = true;
        self.data.u128 = &outside;
        databoxBigCopyFrom(&self, (const databox *)&self);
        outside = 0;
        if (self.data.u128 != (__uint128_t *)self.extra || !self.big ||
            self.allocated || *self.data.u128 != want - 5) {
            ERR("aliased re-home did not move the payload inline");
        }

        /* Narrow values copy as is, with big = 0 and nothing owned. */
        const databox neg = databoxNewSigned(-5);
        memset(&dst, 0xFF, sizeof(dst));
        databoxBigCopyFrom(&dst, &neg);
        if (dst.type != DATABOX_SIGNED_64 || dst.data.i64 != -5 || dst.big ||
            dst.allocated) {
            ERR("narrow re-home is wrong");
        }

        /* Owned BYTES are borrowed, never adopted. */
        databox bytes =
            databoxNewBytesAllocateOrEmbed("a value longer than 8", 21);
        memset(&dst, 0xFF, sizeof(dst));
        databoxBigCopyFrom(&dst, &bytes);
        if (dst.type != DATABOX_BYTES || dst.allocated || dst.big ||
            dst.data.ptr != bytes.data.ptr || dst.len != 21) {
            ERR("BYTES re-home adopted or lost the source bytes");
        }
        databoxFreeData(&bytes);
    }

    TEST("databoxWide: a 16-byte aligned slot a plain box can borrow") {
        struct {
            uint8_t pad;
            databoxWide slot;
        } skewed;
        if (sizeof(databoxWide) != 16 || alignof(databoxWide) != 16 ||
            ((uintptr_t)&skewed.slot % 16) != 0) {
            ERR("databoxWide is not 16 bytes at 16-byte alignment");
        }

        /* A box aimed at a slot carries the full value. */
        databoxWide slot;
        databox box = {};
        box.type = DATABOX_SIGNED_128;
        box.big = true;
        box.data.i128 = (__int128_t *)&slot;
        *box.data.i128 = INT128_MIN + 3;
        if (databoxIsWideDeferred(&box) || *box.data.i128 != INT128_MIN + 3) {
            ERR("a box aimed at a slot reads as deferred");
        }

        /* Wide type with no payload is deferred, for either signedness. */
        const databoxType wideTypes[] = {DATABOX_SIGNED_128,
                                         DATABOX_UNSIGNED_128};
        for (size_t i = 0; i < COUNT_ARRAY(wideTypes); i++) {
            databox deferred;
            memset(&deferred, 0xFF, sizeof(deferred));
            deferred.type = wideTypes[i];
            deferred.big = false;
            deferred.data.ptr = NULL;
            if (!databoxIsWideDeferred(&deferred)) {
                ERR("type %d with a NULL payload is not deferred",
                    (int)wideTypes[i]);
            }
        }

        /* Narrow values are never deferred, even with a zero payload. */
        const databox narrow[] = {databoxNewSigned(0), databoxNewUnsigned(0),
                                  DATABOX_DOUBLE(0.0), databoxNull(),
                                  databoxVoid(),       {{0}}};
        for (size_t i = 0; i < COUNT_ARRAY(narrow); i++) {
            if (databoxIsWideDeferred(&narrow[i])) {
                ERR("narrow box %zu reads as deferred", i);
            }
        }
    }

    TEST("databoxReprStr: wide boxes, with and without a payload") {
        /* A deferred box (0xFF everywhere but the type and a NULL pointer,
         * so 'big' is set too) prints as deferred and is never read
         * through; a box with a payload prints its value. */
        const struct {
            databoxType type;
            const char *deferred;
            const char *live;
        } cases[] = {
            {DATABOX_SIGNED_128, "{SIGNED128: deferred}",
             "{SIGNED128: -170141183460469231731687303715884105728}"},
            {DATABOX_UNSIGNED_128, "{UNSIGNED128: deferred}",
             "{UNSIGNED128: 340282366920938463463374607431768211455}"},
        };

        for (size_t i = 0; i < COUNT_ARRAY(cases); i++) {
            databox deferred;
            memset(&deferred, 0xFF, sizeof(deferred));
            deferred.type = cases[i].type;
            deferred.data.ptr = NULL;
            const char *got = databoxReprStr(&deferred);
            if (strcmp(got, cases[i].deferred) != 0) {
                ERR("deferred type %d printed as %s", (int)cases[i].type, got);
            }

            databoxBig live;
            memset(&live, 0, sizeof(live));
            if (cases[i].type == DATABOX_SIGNED_128) {
                DATABOX_BIG_SIGNED_128(&live, INT128_MIN);
            } else {
                DATABOX_BIG_UNSIGNED_128(&live, UINT128_MAX);
            }

            got = databoxReprStr((const databox *)&live);
            if (strcmp(got, cases[i].live) != 0) {
                ERR("type %d printed as %s", (int)cases[i].type, got);
            }
        }
    }

    TEST("databoxCompare: a deferred wide operand fails fast") {
        databox deferredS = {};
        deferredS.type = DATABOX_SIGNED_128;
        databox deferredU = {};
        deferredU.type = DATABOX_UNSIGNED_128;
        databoxBig full;
        DATABOX_BIG_UNSIGNED_128(&full, (__uint128_t)1 << 100);
        const databox narrowS = databoxNewSigned(-7);
        const databox narrowU = databoxNewUnsigned(7);
        const databox real = DATABOX_DOUBLE(1.5);

        /* Every wide arm, with the deferred box on either side. */
        const struct {
            const databox *a;
            const databox *b;
        } dies[] = {
            {&deferredS, &narrowS},
            {&narrowU, &deferredU},
            {&deferredU, (const databox *)&full},
            {(const databox *)&full, &deferredS},
            {&deferredS, &real},
            {&real, &deferredU},
        };
        for (size_t i = 0; i < COUNT_ARRAY(dies); i++) {
            if (!databoxTestCompareAborts_(dies[i].a, dies[i].b)) {
                ERR("compare case %zu did not abort", i);
            }
        }

        /* A wide box with its payload still compares normally. */
        if (databoxCompare((const databox *)&full, &narrowU) <= 0 ||
            databoxCompare(&narrowS, (const databox *)&full) >= 0) {
            ERR("wide compare with a payload is wrong");
        }
    }

    TEST("databoxCompare: integers of every width and sign by value") {
        /* Boundary values, each boxed in every integer type that can hold
         * it, compared pairwise.  The oracle orders values above INT128_MAX
         * (only unsigned boxes hold them) after every other value, and
         * compares the rest as __int128. */
        const __int128_t two63 = (__int128_t)1 << 63;
        const __int128_t two64 = (__int128_t)1 << 64;
        const __int128_t two96 = (__int128_t)1 << 96;
        const __int128_t signedValues[] = {
            0,
            1,
            -1,
            2,
            -2,
            INT64_MAX,
            INT64_MAX - 1,
            two63,
            two63 + 1,
            INT64_MIN,
            INT64_MIN + 1,
            (__int128_t)INT64_MIN - 1,
            two64 - 1,
            two64 - 2,
            two64,
            two64 + 1,
            -two64,
            -two64 + 1,
            -two64 - 1,
            two96 - 1,
            two96,
            two96 + 1,
            -two96,
            -two96 - 1,
            -two96 + 1,
            INT128_MAX,
            INT128_MAX - 1,
            INT128_MIN,
            INT128_MIN + 1,
        };
        const __uint128_t aboveValues[] = {(__uint128_t)INT128_MAX + 1,
                                           (__uint128_t)INT128_MAX + 2,
                                           UINT128_MAX - 1, UINT128_MAX};
        enum { MATRIX_FORMS_ = 4 };
        enum {
            MATRIX_VALUES_ =
                COUNT_ARRAY(signedValues) + COUNT_ARRAY(aboveValues)
        };

        databoxBig boxes[MATRIX_VALUES_ * MATRIX_FORMS_];
        struct {
            bool above;
            __int128_t s;
            __uint128_t u;
        } oracle[COUNT_ARRAY(boxes)];
        size_t count = 0;
        for (size_t v = 0; v < MATRIX_VALUES_; v++) {
            const bool above = v >= COUNT_ARRAY(signedValues);
            const __int128_t s = above ? 0 : signedValues[v];
            const __uint128_t u =
                above ? aboveValues[v - COUNT_ARRAY(signedValues)]
                      : (__uint128_t)s;
            for (int form = 0; form < MATRIX_FORMS_; form++) {
                databoxBig *const b = &boxes[count];
                memset(b, 0, sizeof(*b));
                if (form == 0 && !above && s >= INT64_MIN && s <= INT64_MAX) {
                    b->type = DATABOX_SIGNED_64;
                    b->data.i = (int64_t)s;
                } else if (form == 1 && !above && s >= 0 && s <= two64 - 1) {
                    b->type = DATABOX_UNSIGNED_64;
                    b->data.u = (uint64_t)s;
                } else if (form == 2 && !above) {
                    DATABOX_BIG_SIGNED_128(b, s);
                } else if (form == 3 && (above || s >= 0)) {
                    DATABOX_BIG_UNSIGNED_128(b, u);
                } else {
                    continue;
                }

                oracle[count].above = above;
                oracle[count].s = s;
                oracle[count].u = u;
                count++;
            }
        }

        size_t pairs = 0;
        for (size_t i = 0; i < count; i++) {
            for (size_t j = 0; j < count; j++) {
                int want;
                if (oracle[i].above != oracle[j].above) {
                    want = oracle[i].above ? 1 : -1;
                } else if (oracle[i].above) {
                    want = oracle[i].u < oracle[j].u   ? -1
                           : oracle[i].u > oracle[j].u ? 1
                                                       : 0;
                } else {
                    want = oracle[i].s < oracle[j].s   ? -1
                           : oracle[i].s > oracle[j].s ? 1
                                                       : 0;
                }

                const int got = databoxCompare((const databox *)&boxes[i],
                                               (const databox *)&boxes[j]);
                if ((got > 0) - (got < 0) != want) {
                    printf("    %s against ",
                           databoxReprStr((const databox *)&boxes[i]));
                    ERR("%s gave %d, expected %d",
                        databoxReprStr((const databox *)&boxes[j]), got, want);
                }
                pairs++;
            }
        }

        /* 9 SIGNED_64, 9 UNSIGNED_64, 29 SIGNED_128 and 20 UNSIGNED_128 */
        if (count != 67) {
            ERR("%zu integer boxes in the matrix, expected 67", count);
        }
        printf("    %zu boxes, %zu ordered pairs\n", count, pairs);
    }

    TEST("databoxCompare: the numeric order is a strict weak order") {
        /* Integers of every width, reals near the 2^24, 2^53, 2^63, 2^64
         * and 2^128 boundaries, +/-0, +/-inf, NaNs with different signs and
         * payloads, and the pointer types that join the unsigned order.
         * Every pair must agree with the exact-value oracle, and the
         * matrix of results must be reflexive, antisymmetric and
         * transitive. */
        databoxBig corpus[96];
        size_t n = 0;
#define DB_ORDER_ADD(box)                                                      \
    do {                                                                       \
        memset(&corpus[n], 0, sizeof(corpus[n]));                              \
        const databox one_ = (box);                                            \
        memcpy(&corpus[n], &one_, sizeof(one_));                               \
        n++;                                                                   \
    } while (0)
#define DB_ORDER_ADD_I128(v)                                                   \
    do {                                                                       \
        memset(&corpus[n], 0, sizeof(corpus[n]));                              \
        DATABOX_BIG_SIGNED_128(&corpus[n], (v));                               \
        n++;                                                                   \
    } while (0)
#define DB_ORDER_ADD_U128(v)                                                   \
    do {                                                                       \
        memset(&corpus[n], 0, sizeof(corpus[n]));                              \
        DATABOX_BIG_UNSIGNED_128(&corpus[n], (v));                             \
        n++;                                                                   \
    } while (0)
#define DB_ORDER_ADD_BITS64(bits)                                              \
    do {                                                                       \
        const uint64_t b_ = (bits);                                            \
        double d_;                                                             \
        memcpy(&d_, &b_, sizeof(d_));                                          \
        DB_ORDER_ADD(DATABOX_DOUBLE(d_));                                      \
    } while (0)
#define DB_ORDER_ADD_FLOAT(f)                                                  \
    do {                                                                       \
        databox f_ = {};                                                       \
        DATABOX_SET_FLOAT(&f_, (f));                                           \
        DB_ORDER_ADD(f_);                                                      \
    } while (0)

        const int64_t two53 = INT64_C(1) << 53;
        const int64_t signeds[] = {0,
                                   1,
                                   -1,
                                   2,
                                   two53 - 1,
                                   two53,
                                   two53 + 1,
                                   -two53,
                                   -two53 - 1,
                                   INT64_MAX,
                                   INT64_MAX - 1023,
                                   INT64_MIN,
                                   INT64_MIN + 1,
                                   16777217};
        for (size_t i = 0; i < COUNT_ARRAY(signeds); i++) {
            DB_ORDER_ADD(databoxNewSigned(signeds[i]));
        }

        const uint64_t unsigneds[] = {UINT64_C(9223372036854775808),
                                      UINT64_C(9223372036854775809), UINT64_MAX,
                                      UINT64_MAX - 2047, 5};
        for (size_t i = 0; i < COUNT_ARRAY(unsigneds); i++) {
            DB_ORDER_ADD(databoxNewUnsigned(unsigneds[i]));
        }

        const __int128_t two64 = (__int128_t)1 << 64;
        DB_ORDER_ADD_U128((__uint128_t)two64);
        DB_ORDER_ADD_U128((__uint128_t)two64 + 1);
        DB_ORDER_ADD_U128((__uint128_t)1 << 100);
        DB_ORDER_ADD_U128(UINT128_MAX);
        DB_ORDER_ADD_U128((__uint128_t)1 << 127);
        DB_ORDER_ADD_U128(7);
        DB_ORDER_ADD_I128(-two64);
        DB_ORDER_ADD_I128(-two64 - 1);
        DB_ORDER_ADD_I128(INT128_MIN);
        DB_ORDER_ADD_I128(INT128_MAX);
        DB_ORDER_ADD_I128(-((__int128_t)1 << 100));
        DB_ORDER_ADD_I128(-7);

        const double doubles[] = {0.5,
                                  -0.5,
                                  0.1,
                                  1.0,
                                  2.0,
                                  7.0,
                                  9007199254740992.0, /* 2^53 */
                                  9007199254740994.0, /* 2^53 + 2 */
                                  -9007199254740992.0,
                                  9223372036854775808.0,  /* 2^63 */
                                  -9223372036854775808.0, /* -2^63 */
                                  18446744073709551616.0, /* 2^64 */
                                  18446744073709549568.0, /* 2^64 - 2048 */
                                  0x1p100,
                                  -0x1p100,
                                  0x1p127,
                                  0x1p128,
                                  -0x1p127,
                                  -0x1p128,
                                  1e300,
                                  -1e300,
                                  4.9e-324,
                                  -4.9e-324};
        for (size_t i = 0; i < COUNT_ARRAY(doubles); i++) {
            DB_ORDER_ADD(DATABOX_DOUBLE(doubles[i]));
        }

        DB_ORDER_ADD_FLOAT(0.5f);
        DB_ORDER_ADD_FLOAT(16777216.0f);
        DB_ORDER_ADD_FLOAT(-3.25f);
        DB_ORDER_ADD_FLOAT(0.1f);
        DB_ORDER_ADD_FLOAT(0.0f);
        DB_ORDER_ADD_FLOAT(-0.0f);
        DB_ORDER_ADD_FLOAT(__builtin_inff());
        DB_ORDER_ADD_FLOAT(-__builtin_inff());
        DB_ORDER_ADD_FLOAT(__builtin_nanf(""));
        DB_ORDER_ADD_BITS64(UINT64_C(0x0000000000000000)); /* +0.0 */
        DB_ORDER_ADD_BITS64(UINT64_C(0x8000000000000000)); /* -0.0 */
        DB_ORDER_ADD_BITS64(UINT64_C(0x7FF0000000000000)); /* +inf */
        DB_ORDER_ADD_BITS64(UINT64_C(0xFFF0000000000000)); /* -inf */
        DB_ORDER_ADD_BITS64(UINT64_C(0x7FF8000000000000)); /* quiet NaN */
        DB_ORDER_ADD_BITS64(UINT64_C(0xFFF8000000000000)); /* -NaN */
        DB_ORDER_ADD_BITS64(UINT64_C(0x7FF0000000000001)); /* payload NaN */
        DB_ORDER_ADD_BITS64(UINT64_C(0xFFFFFFFFFFFFFFFF)); /* all-ones NaN */

        static constexpr char pointee[2] = "p";
        DB_ORDER_ADD(databoxNewPtr(NULL));
        DB_ORDER_ADD(databoxNewPtr((void *)(uintptr_t)5));
        DB_ORDER_ADD(databoxNewPtr((void *)pointee));
        databox reference = {};
        reference.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
        reference.data.u = 7;
        DB_ORDER_ADD(reference);
        reference.data.u = UINT64_MAX;
        DB_ORDER_ADD(reference);
#undef DB_ORDER_ADD_FLOAT
#undef DB_ORDER_ADD_BITS64
#undef DB_ORDER_ADD_U128
#undef DB_ORDER_ADD_I128
#undef DB_ORDER_ADD
        assert(n <= COUNT_ARRAY(corpus));

        int8_t got[COUNT_ARRAY(corpus)][COUNT_ARRAY(corpus)];
        size_t mismatches = 0;
        for (size_t i = 0; i < n; i++) {
            const dbOrderValue_ oi = dbOrderOf_((const databox *)&corpus[i]);
            for (size_t j = 0; j < n; j++) {
                const dbOrderValue_ oj =
                    dbOrderOf_((const databox *)&corpus[j]);
                const int c = databoxCompare((const databox *)&corpus[i],
                                             (const databox *)&corpus[j]);
                got[i][j] = (int8_t)((c > 0) - (c < 0));
                const int want = dbOrderCompare_(&oi, &oj);
                if (got[i][j] != want && mismatches++ < 20) {
                    printf("    %s against ",
                           databoxReprStr((const databox *)&corpus[i]));
                    ERR("%s gave %d, expected %d",
                        databoxReprStr((const databox *)&corpus[j]),
                        (int)got[i][j], want);
                }
            }
        }

        if (mismatches) {
            ERR("%zu pairs disagree with the oracle", mismatches);
        }

        size_t violations = 0;
        for (size_t i = 0; i < n; i++) {
            if (got[i][i] != 0) {
                violations++;
            }

            for (size_t j = 0; j < n; j++) {
                if (got[i][j] != -got[j][i]) {
                    violations++;
                }

                for (size_t k = 0; k < n; k++) {
                    /* a <= b <= c implies a <= c, strictly if either step
                     * is strict. */
                    if (got[i][j] <= 0 && got[j][k] <= 0) {
                        const int want =
                            (got[i][j] < 0 || got[j][k] < 0) ? -1 : 0;
                        if (got[i][k] != want) {
                            violations++;
                        }
                    }
                }
            }
        }

        if (violations) {
            ERR("%zu strict-weak-order violations over %zu values", violations,
                n);
        }
        printf("    %zu values, %zu ordered pairs\n", n, n * n);

        /* databoxNumericHashKey over the same corpus: compare-equal values
         * must share a key.  The corpus holds no unequal pair whose keys
         * may legitimately collide, so any shared key between unequal
         * values is reported too. */
        uint64_t keys[COUNT_ARRAY(corpus)];
        for (size_t i = 0; i < n; i++) {
            if (!databoxNumericHashKey((const databox *)&corpus[i], &keys[i])) {
                ERR("%s has no numeric hash key",
                    databoxReprStr((const databox *)&corpus[i]));
            }
        }

        size_t equalPairs = 0;
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                if (got[i][j] == 0) {
                    equalPairs += i != j;
                    if (keys[i] != keys[j]) {
                        printf("    %s and ",
                               databoxReprStr((const databox *)&corpus[i]));
                        ERR("%s compare equal but key differently",
                            databoxReprStr((const databox *)&corpus[j]));
                    }
                } else if (keys[i] == keys[j]) {
                    printf("    %s and ",
                           databoxReprStr((const databox *)&corpus[i]));
                    ERR("%s differ but share a key",
                        databoxReprStr((const databox *)&corpus[j]));
                }
            }
        }

        /* Bytes and the immediate types have no numeric key. */
        const databox notNumbers[] = {databoxNewBytesString("5"),
                                      DATABOX_BOX_TRUE, DATABOX_BOX_NULL,
                                      DATABOX_BOX_VOID};
        for (size_t i = 0; i < COUNT_ARRAY(notNumbers); i++) {
            uint64_t key = UINT64_C(0xA5A5A5A5A5A5A5A5);
            if (databoxNumericHashKey(&notNumbers[i], &key) ||
                key != UINT64_C(0xA5A5A5A5A5A5A5A5)) {
                ERR("type %d got a numeric hash key", (int)notNumbers[i].type);
            }
        }

        if (equalPairs < 40) {
            ERR("only %zu equal pairs exercised the hash", equalPairs);
        }
        printf("    hash keys: %zu equal ordered pairs share their keys\n",
               equalPairs);
    }

    TEST("databoxCanonicalNumeral: the canonical-numeral table") {
        const __uint128_t two64 = (__uint128_t)1 << 64;
        const struct {
            const char *s;
            databoxType type; /* DATABOX_BYTES: stays bytes */
            int64_t i;
            uint64_t u;
            double d;
            __int128_t wide;
            __uint128_t uwide;
        } rows[] = {
#define DB_CANON_ROW(str, t, field, v) {.s = (str), .type = (t), .field = (v)}
#define DB_CANON_BYTES(str) {.s = (str), .type = DATABOX_BYTES}
            DB_CANON_ROW("22", DATABOX_SIGNED_64, i, 22),
            DB_CANON_ROW("0", DATABOX_SIGNED_64, i, 0),
            DB_CANON_ROW("-5", DATABOX_SIGNED_64, i, -5),
            DB_CANON_ROW("9223372036854775807", DATABOX_SIGNED_64, i,
                         INT64_MAX),
            DB_CANON_ROW("-9223372036854775808", DATABOX_SIGNED_64, i,
                         INT64_MIN),
            DB_CANON_ROW("9223372036854775808", DATABOX_UNSIGNED_64, u,
                         UINT64_C(9223372036854775808)),
            DB_CANON_ROW("18446744073709551615", DATABOX_UNSIGNED_64, u,
                         UINT64_MAX),
            DB_CANON_ROW("22.0", DATABOX_FLOAT_32, d, 22.0),
            DB_CANON_ROW("-0.5", DATABOX_FLOAT_32, d, -0.5),
            DB_CANON_ROW("299.5", DATABOX_FLOAT_32, d, 299.5),
            DB_CANON_ROW("0.0", DATABOX_FLOAT_32, d, 0.0),
            DB_CANON_ROW("0.4", DATABOX_DOUBLE_64, d, 0.4),
            DB_CANON_ROW("18446744073709551616", DATABOX_UNSIGNED_128, uwide,
                         two64),
            DB_CANON_ROW("-9223372036854775809", DATABOX_SIGNED_128, wide,
                         (__int128_t)INT64_MIN - 1),
            DB_CANON_ROW("-18446744073709551616", DATABOX_SIGNED_128, wide,
                         -(__int128_t)two64),
            DB_CANON_ROW("170141183460469231731687303715884105727",
                         DATABOX_UNSIGNED_128, uwide, (__uint128_t)INT128_MAX),
            DB_CANON_ROW("170141183460469231731687303715884105728",
                         DATABOX_UNSIGNED_128, uwide,
                         (__uint128_t)INT128_MAX + 1),
            DB_CANON_ROW("-170141183460469231731687303715884105728",
                         DATABOX_SIGNED_128, wide, INT128_MIN),
            DB_CANON_ROW("340282366920938463463374607431768211455",
                         DATABOX_UNSIGNED_128, uwide, UINT128_MAX),
            DB_CANON_BYTES(""),
            DB_CANON_BYTES("007"),
            DB_CANON_BYTES("01"),
            DB_CANON_BYTES("-0"),
            DB_CANON_BYTES("-01"),
            DB_CANON_BYTES("+5"),
            DB_CANON_BYTES(" 22"),
            DB_CANON_BYTES("22 "),
            DB_CANON_BYTES("1e3"),
            DB_CANON_BYTES(".5"),
            DB_CANON_BYTES("5."),
            DB_CANON_BYTES("0."),
            DB_CANON_BYTES("-"),
            DB_CANON_BYTES("."),
            DB_CANON_BYTES("-0.0"),
            DB_CANON_BYTES("299.5000"),
            DB_CANON_BYTES("123.10"),
            DB_CANON_BYTES("0x10"),
            DB_CANON_BYTES("1,000"),
            DB_CANON_BYTES("nan"),
            DB_CANON_BYTES("inf"),
            DB_CANON_BYTES("-inf"),
            DB_CANON_BYTES("0018446744073709551616"),
            DB_CANON_BYTES("-018446744073709551616"),
            DB_CANON_BYTES("340282366920938463463374607431768211456"),
            DB_CANON_BYTES("-170141183460469231731687303715884105729"),
            DB_CANON_BYTES("1000000000000000000000000000000000000000"),
#undef DB_CANON_BYTES
#undef DB_CANON_ROW
        };

        for (size_t r = 0; r < COUNT_ARRAY(rows); r++) {
            const size_t len = strlen(rows[r].s);
            const databox in = {.data.bytes.ccstart = rows[r].s,
                                .type = DATABOX_BYTES,
                                .len = len};
            databoxBig out = {};
            const bool got = databoxCanonicalNumeral(&in, &out);
            databoxBig scratch = {};
            const databox *const use = databoxCanonical(&in, &scratch);
            if (rows[r].type == DATABOX_BYTES) {
                if (got || use != &in) {
                    ERR("'%s' should stay bytes, got %s", rows[r].s,
                        databoxReprStr((const databox *)&out));
                }
                continue;
            }

            bool same = got && use == (const databox *)&scratch &&
                        out.type == rows[r].type &&
                        dbCanonSame_((const databox *)&out, use);
            if (same) {
                switch (rows[r].type) {
                case DATABOX_SIGNED_64:
                    same = out.data.i == rows[r].i;
                    break;
                case DATABOX_UNSIGNED_64:
                    same = out.data.u == rows[r].u;
                    break;
                case DATABOX_FLOAT_32: {
                    const float want = (float)rows[r].d;
                    same = memcmp(&out.data.f32, &want, sizeof(want)) == 0;
                    break;
                }
                case DATABOX_DOUBLE_64:
                    same = out.data.d64 == rows[r].d;
                    break;
                case DATABOX_SIGNED_128:
                    same = *out.data.i128 == rows[r].wide;
                    break;
                case DATABOX_UNSIGNED_128:
                    same = *out.data.u128 == rows[r].uwide;
                    break;
                default:
                    same = false;
                    break;
                }
            }

            if (!same) {
                ERR("'%s' canonicalized to %s (returned %d)", rows[r].s,
                    databoxReprStr((const databox *)&out), (int)got);
            }
        }

        /* Only BYTES and BYTES_EMBED are parsed as numerals, and only
         * they and whole reals change under databoxCanonical; every other
         * type, a fractional real included, passes through.  A whole real
         * is not a numeral: databoxCanonicalNumeral leaves it, and
         * databoxCanonical makes it its integer. */
        const databox real22 = DATABOX_DOUBLE(22.0);
        databoxBig wholeOut;
        databoxBig untouched;
        memset(&untouched, 0xA5, sizeof(untouched));
        const databoxBig poisoned = untouched;
        const databox *real22Use = databoxCanonical(&real22, &wholeOut);
        if (databoxCanonicalNumeral(&real22, &untouched) ||
            memcmp(&untouched, &poisoned, sizeof(untouched)) != 0 ||
            real22Use->type != DATABOX_UNSIGNED_64 || real22Use->data.u != 22) {
            ERR("a whole real was parsed as a numeral or not made an "
                "integer");
        }
        static constexpr char twentyTwo[] = "22";
        const databox others[] = {
            databoxNewSigned(22),
            databoxNewUnsigned(22),
            DATABOX_DOUBLE(22.5),
            DATABOX_BOX_TRUE,
            DATABOX_BOX_NULL,
            databoxNewPtr((void *)twentyTwo),
            {.data.bytes.ccstart = twentyTwo,
             .type = DATABOX_BYTES_NEVER_FREE,
             .len = 2},
        };
        for (size_t o = 0; o < COUNT_ARRAY(others); o++) {
            databoxBig out;
            memset(&out, 0xA5, sizeof(out));
            const databoxBig poison = out;
            if (databoxCanonicalNumeral(&others[o], &out) ||
                memcmp(&out, &poison, sizeof(out)) != 0 ||
                databoxCanonical(&others[o], &out) != &others[o]) {
                ERR("type %d was canonicalized", (int)others[o].type);
            }
        }
    }

    TEST("databoxCanonicalNumeral: differential over integer boundaries and "
         "near-numerals") {
        dbCanonTally_ tally = {};
        char spelled[48];

        /* Each width's boundaries, one either side, with both signs: the
         * signed maximum and minimum and the unsigned maximum. */
        static constexpr uint32_t widths[] = {8, 16, 32, 64, 128};
        for (size_t w = 0; w < COUNT_ARRAY(widths); w++) {
            const uint32_t bits = widths[w];
            const __uint128_t half = (__uint128_t)1 << (bits - 1);
            const __uint128_t full =
                bits == 128 ? UINT128_MAX : ((__uint128_t)1 << bits) - 1;
            const __uint128_t magnitudes[] = {half - 2, half - 1, half,
                                              half + 1, full - 1, full};
            for (size_t m = 0; m < COUNT_ARRAY(magnitudes); m++) {
                for (int negative = 0; negative < 2; negative++) {
                    dbCanonSpell_(spelled, magnitudes[m], negative);
                    err +=
                        dbCanonCheck_(spelled, strlen(spelled), false, &tally);
                    err += dbCanonMutations_(spelled, &tally);
                }
            }

            if (bits < 128) {
                for (int negative = 0; negative < 2; negative++) {
                    dbCanonSpell_(spelled, full + 1, negative);
                    err +=
                        dbCanonCheck_(spelled, strlen(spelled), false, &tally);
                    err += dbCanonMutations_(spelled, &tally);
                }
            }
        }

        /* Past 128 bits, which no 128-bit value can spell. */
        static const char *const beyond[] = {
            "340282366920938463463374607431768211456",
            "340282366920938463463374607431768211457",
            "-340282366920938463463374607431768211456",
            "999999999999999999999999999999999999999",
            "-999999999999999999999999999999999999999",
            "1000000000000000000000000000000000000000",
            "-1000000000000000000000000000000000000000",
            "10000000000000000000000000000000000000000",
        };
        for (size_t b = 0; b < COUNT_ARRAY(beyond); b++) {
            err += dbCanonCheck_(beyond[b], strlen(beyond[b]), false, &tally);
            err += dbCanonMutations_(beyond[b], &tally);
        }

        /* Decimal-length boundaries: 10^k - 1, 10^k and 10^k + 1. */
        __uint128_t power = 1;
        for (int k = 0; k <= 38; k++) {
            const __uint128_t near[] = {power - 1, power, power + 1};
            for (size_t n = 0; n < COUNT_ARRAY(near); n++) {
                for (int negative = 0; negative < 2; negative++) {
                    dbCanonSpell_(spelled, near[n], negative);
                    err +=
                        dbCanonCheck_(spelled, strlen(spelled), false, &tally);
                }
            }
            power *= 10;
        }

        /* Every value at every width and both signs was exercised, every
         * 64-bit form appeared, and the 128-bit arm was reached. */
        if (tally.narrow < 100 || tally.wide < 40 || tally.bytes < 1000 ||
            tally.reals < 20) {
            ERR("corpus too thin: %" PRIu32 " narrow, %" PRIu32
                " wide, %" PRIu32 " reals, %" PRIu32 " bytes",
                tally.narrow, tally.wide, tally.reals, tally.bytes);
        }
        printf("    %" PRIu32 " strings: %" PRIu32 " narrow, %" PRIu32
               " wide, %" PRIu32 " reals, %" PRIu32 " bytes\n",
               tally.strings, tally.narrow, tally.wide, tally.reals,
               tally.bytes);
    }

    TEST("RetainBytesSelfExact, GetSizeMinimum, NewOffsetAllowEmbed, Free") {
        /* A short value embeds and frees the handed buffer; a long one
         * moves into it (ASan watches both buffers) */
        const char *shortText = "abc";
        databox small = databoxNewBytes(shortText, 3);
        /* @own: container; free=databoxRetainBytesSelfExact */
        void *smallBuf = zmalloc(3);
        databoxRetainBytesSelfExact(&small, smallBuf);
        const char *longText = "a value longer than any embed";
        databox big = databoxNewBytes(longText, strlen(longText));
        /* @own: container; free=databoxFreeData */
        void *bigBuf = zmalloc(strlen(longText));
        databoxRetainBytesSelfExact(&big, bigBuf);
        if (small.type != DATABOX_BYTES_EMBED ||
            memcmp(databoxBytes(&small), "abc", 3) != 0 || !big.allocated ||
            databoxBytes(&big) == (const uint8_t *)longText ||
            memcmp(databoxBytes(&big), longText, big.len) != 0) {
            ERR("RetainBytesSelfExact did not take its value");
        }
        databoxFreeData(&small);
        databoxFreeData(&big);

        /* Every type has a payload size */
        databoxBig wide;
        DATABOX_BIG_SIGNED_128(&wide, -((__int128_t)1 << 90));
        const databox ptr = databoxNewPtr(&wide);
        const databox flag = DATABOX_BOX_TRUE;
        const databox real = DATABOX_DOUBLE(1.5);
        const databox bytes = databoxNewBytesString("hello");
        if (databoxGetSizeMinimum((databox *)&wide) != 16 ||
            databoxGetSizeMinimum(&ptr) != sizeof(void *) ||
            databoxGetSizeMinimum(&flag) != 0 ||
            databoxGetSizeMinimum(&real) != 8 ||
            databoxGetSizeMinimum(&bytes) != 5) {
            ERR("GetSizeMinimum gave a wrong size");
        }

        /* A short slice of a base embeds its bytes; a long one keeps the
         * offset into the base */
        const char base[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        const databox shortSlice = databoxNewOffsetAllowEmbed(base, 10, 4);
        const databox longSlice = databoxNewOffsetAllowEmbed(base, 3, 30);
        if (shortSlice.type != DATABOX_BYTES_EMBED || shortSlice.len != 4 ||
            memcmp(databoxBytes(&shortSlice), "abcd", 4) != 0 ||
            longSlice.type != DATABOX_BYTES_OFFSET || longSlice.len != 30 ||
            longSlice.data.bytes.offset != 3) {
            ERR("NewOffsetAllowEmbed built the wrong box");
        }

        /* databoxFree releases the box and what it owns */
        databox *heap = zcalloc(1, sizeof(*heap));
        databoxCopyBytes(heap, longText, strlen(longText));
        databoxFree(heap);
        databoxFree(NULL);
    }

    TEST("databoxCanonicalNumeral: real-shaped strings follow the scanner") {
        dbCanonTally_ tally = {};
        uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
        char s[64];
        for (int i = 0; i < 20000; i++) {
            size_t at = 0;
            const uint64_t r = dbCanonNext_(&state);
            if (r & 1) {
                s[at++] = '-';
            }

            /* 0 to 16 integer digits, then 1 to 17 fraction digits. */
            const uint32_t intDigits = (uint32_t)((r >> 1) % 17);
            if (intDigits == 0) {
                s[at++] = '0';
            } else {
                s[at++] = (char)('1' + dbCanonNext_(&state) % 9);
                for (uint32_t k = 1; k < intDigits; k++) {
                    s[at++] = (char)('0' + dbCanonNext_(&state) % 10);
                }
            }

            s[at++] = '.';
            const uint32_t fracDigits = 1 + (uint32_t)((r >> 8) % 17);
            for (uint32_t k = 0; k < fracDigits; k++) {
                s[at++] = (char)('0' + dbCanonNext_(&state) % 10);
            }

            err += dbCanonCheck_(s, at, false, &tally);
        }

        /* Values the formatter prints, which the scanner should mostly take
         * back. */
        for (int i = 1; i <= 2000; i++) {
            const double v = (i & 1 ? -1.0 : 1.0) * (double)i / 64.0;
            const size_t n = StrDoubleFormatToBufNice(s, sizeof(s), v);
            err += dbCanonCheck_(s, n, false, &tally);
        }

        if (tally.reals < 2000) {
            ERR("only %" PRIu32 " of %" PRIu32 " real-shaped strings scanned",
                tally.reals, tally.strings);
        }
        printf("    %" PRIu32 " strings: %" PRIu32 " reals, %" PRIu32
               " bytes\n",
               tally.strings, tally.reals, tally.bytes);
    }

    TEST("databoxCanonical: the fast reject never rejects a numeral") {
        /* Every first byte in front of tails that make a numeral with a
         * digit or '-' in front, and near-numerals otherwise. */
        static const char *const tails[] = {
            "",
            "1",
            "0",
            "5.5",
            "22",
            "18446744073709551616",
            "170141183460469231731687303715884105727"};
        dbCanonTally_ tally = {};
        char s[64];
        for (uint32_t c = 0; c < 256; c++) {
            for (size_t t = 0; t < COUNT_ARRAY(tails); t++) {
                const size_t tailLen = strlen(tails[t]);
                s[0] = (char)c;
                memcpy(s + 1, tails[t], tailLen);
                err += dbCanonCheck_(s, tailLen + 1, false, &tally);
            }
        }

        if (tally.narrow == 0 || tally.wide == 0 || tally.reals == 0) {
            ERR("the first-byte sweep reached no numeral");
        }
    }

    TEST("databoxFreeData: a wide payload is freed by 'allocated', never by "
         "address") {
        datakitConfig saved = datakitConfigMemory__;
        datakitConfig recording = saved;
        recording.localFree = dbFreeRecord_;

        /* An array of two plain boxes: [0] owns a "payload" that sits
         * exactly where a databoxBig would keep its inline value, as a
         * one-entry rangeBox array and a heap payload allocated next to
         * it can.  The free must still happen. */
        databox pair[2];
        memset(pair, 0, sizeof(pair));
        pair[0].type = DATABOX_UNSIGNED_128;
        pair[0].big = true;
        pair[0].allocated = true;
        pair[0].data.ptr = &pair[1];

        dbFreeRecorded_ = NULL;
        dbFreeCalls_ = 0;
        datakitConfigSet(&recording);
        databoxFreeData(&pair[0]);
        datakitConfigSet(&saved);
        if (dbFreeCalls_ != 1 || dbFreeRecorded_ != (const void *)&pair[1] ||
            pair[0].allocated || pair[0].data.ptr) {
            ERR("an owned wide payload next to its box was not freed");
        }

        /* DATABOX_BIG_INIT over stale bits owns nothing: freeing the box
         * leaves its inline value alone. */
        databoxBig armed;
        memset(&armed, 0xFF, sizeof(armed));
        DATABOX_BIG_INIT(&armed);
        armed.type = DATABOX_SIGNED_128;
        dbFreeCalls_ = 0;
        datakitConfigSet(&recording);
        databoxFreeData((databox *)&armed);
        datakitConfigSet(&saved);
        if (armed.allocated || dbFreeCalls_ != 0 ||
            armed.data.ptr != (void *)armed.extra) {
            ERR("DATABOX_BIG_INIT left the box claiming its inline value");
        }
    }

    printf("\n=== All databox fuzz tests completed! ===\n\n");

    TEST("Repr: every kind of box prints without faults") {
        databoxBig wide;
        DATABOX_BIG_UNSIGNED_128(&wide, ((__uint128_t)1 << 100) + 7);
        const databox boxes[] = {
            databoxNewSigned(-5),
            databoxNewUnsigned(7),
            DATABOX_DOUBLE(1.5),
            {.type = DATABOX_FLOAT_32, .data.f32 = 0.5f},
            databoxNewBytesString("text"),
            databoxNewBytesAllowEmbed("hi", 2),
            DATABOX_BOX_TRUE,
            DATABOX_BOX_FALSE,
            DATABOX_BOX_NULL,
            DATABOX_BOX_VOID,
            {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL, .data.u = 9},
            *(const databox *)&wide,
        };
        for (size_t i = 0; i < COUNT_ARRAY(boxes); i++) {
            databoxRepr(&boxes[i]);
            databoxReprSay("box:", &boxes[i]);
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
