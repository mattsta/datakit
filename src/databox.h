#pragma once

#include "datakit.h"

#ifndef DATABOX_ENABLE_PTR_MDSC
#define DATABOX_ENABLE_PTR_MDSC 1
#endif

typedef enum databoxType {
    /** Fixed-length types **/
    /* Invalid databox is all zeroes */
    DATABOX_VOID = 0, /* void also counts as UNDEFINED */

    /* A user-defined error condition */
    DATABOX_ERROR = 1,

    /* ================================================================= */
    /* NOTE: Order of the four {UN,}SIGNED_{64,128}, FLOAT, DOUBLE matter!
     * Keep them at these offsets inside databoxType. */
    /* Immediate 8 byte types inside a databox */
    DATABOX_SIGNED_64 = 2,
    DATABOX_UNSIGNED_64 = 3,

    /* 16 byte types for databoxBig */
    DATABOX_SIGNED_128 = 4,
    DATABOX_UNSIGNED_128 = 5,

    /* Immediate 4 or 8 byte types inside a databox */
    DATABOX_FLOAT_32 = 6,
    DATABOX_DOUBLE_64 = 7,
    /* ================================================================= */

    /* Linear data structure markers */
    DATABOX_ARRAY_START = 8,
    DATABOX_ARRAY_END = 9,
    DATABOX_MAP_START = 10, /* implicit end because Key-Value */
    DATABOX_LIST_START = 11,
    DATABOX_LIST_END = 12,

    /* Immediate type-only values, no data */
    DATABOX_TRUE = 13,
    DATABOX_FALSE = 14,
    DATABOX_NULL = 15,

    /* Pointer value */
    DATABOX_PTR = 16,

    /* Container types */
    /* Note: this is in the correct position because we use
     *       'type >= BYTES' for length checking. */
    DATABOX_CONTAINER_REFERENCE_EXTERNAL = 17, /* is unsigned */

    /** Variable-length types **/
    /* Pointer to bytes we don't own. */
    DATABOX_BYTES = 18,

    /* We have <= 8 bytes stored directly in data.embed. */
    DATABOX_BYTES_EMBED = 19,

    /* User requests allocation of space, but not copy of any
     * contents. Used for pre-allocation of heap-like things. */
    DATABOX_BYTES_VOID = 20,

    /* Pointer to bytes we may have allocated, but don't
     * want to free because they are shared and/or should
     * live forever. */
    DATABOX_BYTES_NEVER_FREE = 21,

    /* Pointer to offset into some other stream of bytes.
     * Not usable as a string of bytes alone, but conceptually
     * represents intent to use as a byte string eventually. */
    DATABOX_BYTES_OFFSET = 22,

    /* Embedded flex aggregates */
    DATABOX_CONTAINER_FLEX_MAP,
    DATABOX_CONTAINER_FLEX_LIST,
    DATABOX_CONTAINER_FLEX_SET,
    DATABOX_CONTAINER_FLEX_TUPLE,

    /* Embedded cflex aggregates */
    DATABOX_CONTAINER_CFLEX_MAP,
    DATABOX_CONTAINER_CFLEX_LIST,
    DATABOX_CONTAINER_CFLEX_SET,
    DATABOX_CONTAINER_CFLEX_TUPLE,

    DATABOX_MAX_EMBED, /* placeholder type for math */

#if DATABOX_ENABLE_PTR_MDSC
    DATABOX_PTR_MDSC,
#endif

    DATABOX_MAX = 255 /* databoxType is only ONE BYTE */
} databoxType;

#ifndef INT128_MIN
#define INT128_MIN                                                             \
    ((__int128_t)0 - ((__int128_t)1 << 126) - ((__int128_t)1 << 126))
#endif

#ifndef INT128_MAX
#define INT128_MAX                                                             \
    ((__int128_t) - 1 + ((__int128_t)1 << 126) + ((__int128_t)1 << 126))
#endif

#ifndef UINT128_MAX
#define UINT128_MAX                                                            \
    (((__uint128_t)1 << 127) - (__uint128_t)1 + ((__uint128_t)1 << 127))
#endif

typedef struct databoxRetainCache {
    /* bytes[0] is a 128 byte memory area then each memory area doubles */
    uint8_t *bytes[16];
    /* 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072,
     * 262144, 524288, 1048576, 2097152, 4194304 */
} databoxRetainCache;

typedef union databoxUnion {
    /* Basic integer access: */
    int8_t i8;
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    intmax_t i;
    uintmax_t u;
    float f32;
    double d64;

    /* Special access cases: retrieving 16 byte integers: */
    __int128_t *i128;
    __uint128_t *u128;

    /* Various ways to store and retrieve bytes: */
    union {
        const char *ccstart;
        char *cstart;
        char cembed[8];
        const uint8_t *custart; /* should just rename it custard */
        uint8_t *start;
        uint8_t embed[8];
        uint8_t embed4[4]; /* can also use to extract bytes of types */
        uint8_t embed8[8]; /* have tiny bytes? store directly. */
        size_t offset;
    } bytes;

    /* Pointer shorthands: */
    void *ptr;
    uintptr_t uptr;
} databoxUnion;

#if DK_C11
_Static_assert(sizeof(databoxUnion) == 8, "databox value too big!");
#endif

/* Bit width of the databox length field and the largest length it can
 * hold.  Longer lengths would be truncated, so callers taking a length
 * from untrusted input must reject anything above DATABOX_BYTES_LEN_MAX.
 * DATABOX_LEN_BITS is fixed by the databox layout below. */
#define DATABOX_LEN_BITS 48
#define DATABOX_BYTES_LEN_MAX ((UINT64_C(1) << DATABOX_LEN_BITS) - 1)

/* Macro-ize common databox metadata because we must share _exactly_ the same
 * layout and fields between 'databox' and 'databoxBig' */
#define databoxInternals_                                                      \
    uint64_t type : 8;      /* databoxtype; valid types start at offset 1 */   \
    uint64_t allocated : 1; /* bool; true if need to free data.bytes.start */  \
    uint64_t created : 1;   /* bool; if retrieved+created, true. */            \
    uint64_t big : 1;       /* bool; true if actually a databoxBig */          \
    uint64_t unused : 5;                                                       \
    uint64_t len : DATABOX_LEN_BITS /* length of 'data.bytes' (281 TB) */

/* databox is a 8 + 8 = 16 byte struct. */
typedef struct databox {
    databoxUnion data; /* 8 bytes */
    databoxInternals_;
} databox;
#if DK_C11
_Static_assert(sizeof(databox) == 16, "databox struct too big!");
#endif

/* 'databoxBig' is 32 bytes total and 16-byte aligned.
 *
 * 'extra' is the inline store for a 96/128-bit value: DATABOX_BIG_INIT
 * aims data.i128 / data.u128 at it, and decoders write the value through
 * that pointer as a full __int128_t / __uint128_t, which needs 16-byte
 * alignment.  The leading 16 bytes are layout-compatible with 'databox',
 * so a databoxBig may be passed wherever a 'databox *' is expected. */
typedef struct databoxBig {
    databoxUnion data;                               /* 8 bytes */
    databoxInternals_;                               /* 8 bytes */
    _Alignas(16) uint8_t extra[sizeof(__uint128_t)]; /* 16 bytes */
} databoxBig;
#if DK_C11
_Static_assert(sizeof(__int128_t) == sizeof(__uint128_t),
               "Your 128 bit integers are weird?");
_Static_assert(sizeof(databoxBig) == 32, "databoxBig struct too big!");
_Static_assert(_Alignof(databoxBig) == 16,
               "databoxBig must be 16-byte aligned for its 128-bit store");
_Static_assert(offsetof(databoxBig, extra) % 16 == 0,
               "databoxBig.extra must be 16-byte aligned");
_Static_assert(offsetof(databoxBig, extra) == sizeof(databox),
               "databoxBig must begin with a complete databox");
#endif

/* 96/128-bit values and the 'big' flag.
 *
 * A wide value does not fit the 8-byte data union, so data.i128 /
 * data.u128 points at it and 'big' records that the pointer is live
 * (aimed at 16 bytes of storage: a databoxBig's 'extra', a databoxWide
 * slot, or a heap payload).  Decoders read 'big' on the destination: with
 * big = 1 they write the value through the pointer; with big = 0 they
 * report only the type and set data.ptr = NULL (databoxIsWideDeferred).
 *
 * The rule: whoever owns the destination storage establishes its wide
 * capability before decoding.  A caller passing its own box sets it up
 * with DATABOX_BIG_INIT on a databoxBig (or zeroes it to accept the type
 * only); a box that is not initialized is a caller bug, because stale
 * 'big' bits turn into a write through a stale pointer.  Library-filled
 * results and library temporaries always carry their own wide storage.
 *
 * DATABOX_BIG_INIT aims the pointer at dbig->extra, sets big = 1 and
 * clears 'allocated': storage the box holds in its own 'extra' is owned
 * by nobody, so databoxFreeData leaves it alone.  It leaves every other
 * field as it was.  A narrow decode overwrites the pointer, so re-run it
 * before each decode into the same box.
 *
 * Ownership is the 'allocated' bit alone: a wide box with allocated = 1
 * owns a heap payload, whatever its address. */
#define DATABOX_BIG_INIT(dbig)                                                 \
    do {                                                                       \
        (dbig)->big = true;                                                    \
        (dbig)->allocated = false;                                             \
        (dbig)->data.u128 = (__uint128_t *)(dbig)->extra;                      \
    } while (0)

/* 'databoxWide' is 16 bytes of 16-byte-aligned storage for one 96/128-bit
 * payload.  It is a slot, not a box: a decoder that is handed a slot aims
 * a plain databox's data.i128 / data.u128 at it, with big = 1 and
 * allocated = 0, so the box borrows the slot and the value is valid for
 * as long as the slot is.  Narrow values never touch the slot, so a slot
 * on the stack or at the tail of an iterator costs nothing until a wide
 * value arrives. */
typedef struct databoxWide {
    _Alignas(16) uint8_t b[sizeof(__uint128_t)];
} databoxWide;
#if DK_C11
_Static_assert(sizeof(databoxWide) == 16, "databoxWide must be 16 bytes");
_Static_assert(_Alignof(databoxWide) == 16,
               "databoxWide must be 16-byte aligned for its 128-bit store");
#endif

/* A deferred wide box has a 96/128-bit type and no payload: data.ptr is
 * NULL.  A decode reports a wide value this way when its destination has
 * no wide storage, so the caller still learns the type and can read the
 * value again into storage it owns.  There is no flag bit for this state;
 * the type and the NULL pointer identify it. */
static inline bool databoxIsWideDeferred(const databox *box) {
    return (box->type == DATABOX_SIGNED_128 ||
            box->type == DATABOX_UNSIGNED_128) &&
           !box->data.ptr;
}

/* Fail fast on a wide box without a payload.  Comparisons, inserts and
 * encoders read a 96/128-bit value through data.i128 / data.u128, and a
 * deferred box has nothing there, so reaching one of those paths with it
 * is a caller bug.  databoxWidePayloadRequire, used only inside their
 * 96/128-bit arms, prints the caller's name and aborts instead of
 * dereferencing NULL.  Narrow values never reach it. */
__attribute__((noreturn, cold)) void
databoxWidePayloadMissing(const databox *box, const char *where);
#define databoxWidePayloadRequire(box)                                         \
    do {                                                                       \
        if (unlikely(!(box)->data.ptr)) {                                      \
            databoxWidePayloadMissing((box), __func__);                        \
        }                                                                      \
    } while (0)

/* Growable databox where databoxStr.box.data.ptr == databoxStr.bytes */
typedef struct databoxStr {
    databox box;
    uint8_t bytes[];
} databoxStr;

/* Bytes a box's value takes, as a fill estimate: its length for the byte
 * types, 16 for a 96/128-bit integer, 8 for anything else */
#define DATABOX_SIZE(box)                                                      \
    (((box)->type >= DATABOX_BYTES) ? (box)->len                               \
     : ((box)->type == DATABOX_SIGNED_128 ||                                   \
        (box)->type == DATABOX_UNSIGNED_128)                                   \
         ? 16                                                                  \
         : 8)

#define DATABOX_CONTAINER_OFFSET(encoding)                                     \
    ((encoding) - DATABOX_CONTAINER_FLEX_MAP)

/* C99 Compound Literals */
/* These look nice, but the compiler implements them by creating a temporary
 * struct then memcpy'ing the temp struct back to the assigned struct.
 * Not always the most efficient. */
#define DATABOX_WITH_BYTES(b, l)                                               \
    (databox) {                                                                \
        .data.bytes.start = (b), .type = DATABOX_BYTES, .len = (l)             \
    }

/* The DK_*_64 constants are IEEE bit patterns, so they go in through the
 * u64 view of the value (an integer assigned to d64 would convert). */
#define DATABOX_NAN                                                            \
    (databox) {                                                                \
        .data.u64 = DK_NAN_64, .type = DATABOX_DOUBLE_64                       \
    }

#define DATABOX_INFINITY_POSITIVE                                              \
    (databox) {                                                                \
        .data.u64 = DK_INFINITY_POSITIVE_64, .type = DATABOX_DOUBLE_64         \
    }

#define DATABOX_INFINITY_NEGATIVE                                              \
    (databox) {                                                                \
        .data.u64 = DK_INFINITY_NEGATIVE_64, .type = DATABOX_DOUBLE_64         \
    }

#define DATABOX_DOUBLE(d)                                                      \
    (databox) {                                                                \
        .data.d64 = (d), .type = DATABOX_DOUBLE_64                             \
    }

#define DATABOX_SIGNED(d)                                                      \
    (databox) {                                                                \
        .data.i64 = (d), .type = DATABOX_SIGNED_64                             \
    }

#define DATABOX_BIG_SIGNED(d)                                                  \
    (databoxBig) {                                                             \
        .data.i64 = (d), .type = DATABOX_SIGNED_64                             \
    }

#define DATABOX_BIG_SIGNED_128(box, d)                                         \
    do {                                                                       \
        DATABOX_BIG_INIT(box);                                                 \
        (box)->type = DATABOX_SIGNED_128;                                      \
        *(box)->data.i128 = d;                                                 \
    } while (0)

#define DATABOX_UNSIGNED(d)                                                    \
    (databox) {                                                                \
        .data.u64 = (d), .type = DATABOX_UNSIGNED_64                           \
    }

#define DATABOX_BIG_UNSIGNED(d)                                                \
    (databoxBig) {                                                             \
        .data.u64 = (d), .type = DATABOX_UNSIGNED_64                           \
    }

#define DATABOX_BIG_UNSIGNED_128(box, d)                                       \
    do {                                                                       \
        DATABOX_BIG_INIT(box);                                                 \
        (box)->type = DATABOX_UNSIGNED_128;                                    \
        *(box)->data.u128 = d;                                                 \
    } while (0)

/* Direct Assignment */
#define DATABOX_SET_NAN(box)                                                   \
    do {                                                                       \
        (box)->data.u64 = DK_NAN_64;                                           \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_INFINITY_POSITIVE(box)                                     \
    do {                                                                       \
        (box)->data.u64 = DK_INFINITY_POSITIVE_64;                             \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_INFINITY_NEGATIVE(box)                                     \
    do {                                                                       \
        (box)->data.u64 = DK_INFINITY_NEGATIVE_64;                             \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_FLOAT(box, d)                                              \
    do {                                                                       \
        (box)->data.f32 = (d);                                                 \
        (box)->type = DATABOX_FLOAT_32;                                        \
    } while (0)

#define DATABOX_SET_DOUBLE(box, d)                                             \
    do {                                                                       \
        (box)->data.d64 = (d);                                                 \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_SIGNED(box, d)                                             \
    do {                                                                       \
        (box)->data.i64 = (d);                                                 \
        (box)->type = DATABOX_SIGNED_64;                                       \
    } while (0)

#define DATABOX_SET_UNSIGNED(box, d)                                           \
    do {                                                                       \
        (box)->data.u64 = (d);                                                 \
        (box)->type = DATABOX_UNSIGNED_64;                                     \
    } while (0)

#define DATABOX_SET_BYTES_OFFSET(box, d)                                       \
    do {                                                                       \
        (box)->data.bytes.offset = (d);                                        \
        (box)->type = DATABOX_BYTES_OFFSET;                                    \
    } while (0)

#define DATABOX_IS_BYTES_EMBED(box) ((box)->type == DATABOX_BYTES_EMBED)
#define DATABOX_IS_TRUE(box) ((box)->type == DATABOX_TRUE)
#define DATABOX_IS_FALSE(box) ((box)->type == DATABOX_FALSE)
#define DATABOX_IS_NULL(box) ((box)->type == DATABOX_NULL)
#define DATABOX_IS_VOID(box) ((box)->type == DATABOX_VOID)

#define DATABOX_IS_FIXED(box)                                                  \
    ((box)->type <= DATABOX_NULL || DATABOX_IS_BYTES_EMBED(box))

#define DATABOX_IS_PTR(box) ((box)->type == DATABOX_PTR)
#define DATABOX_IS_REF(box)                                                    \
    ((box)->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL)

/* 'VOID' is neither true or false; NULL is false; TRUE or != 0 is true */
/* these two should always be inverses of each other. */
#define DATABOX_IS_TRUEISH(box)                                                \
    (DATABOX_IS_TRUE(box) ||                                                   \
     (!DATABOX_IS_VOID(box) && !DATABOX_IS_NULL(box) && (box)->data.u != 0))
#define DATABOX_IS_FALSEISH(box)                                               \
    (DATABOX_IS_FALSE(box) || DATABOX_IS_NULL(box) ||                          \
     (!DATABOX_IS_VOID(box) && (box)->data.u == 0))

/* This should be unnecessary because compilers are smart enough to transform
 * double equals ors for us. */
#define DATABOX_USE_RANGE_CHECK 0
#if DATABOX_USE_RANGE_CHECK
#define databoxCheckRange(low, high, check)                                    \
    (((check) - (low)) <= ((high) - (low)))
#endif

#if DATABOX_USE_RANGE_CHECK
#define DATABOX_IS_BYTES(box)                                                  \
    databoxCheckRange(DATABOX_BYTES, DATABOX_BYTES_EMBED, (box)->type)
#else
#define DATABOX_IS_BYTES(box)                                                  \
    (((box)->type == DATABOX_BYTES) || ((box)->type == DATABOX_BYTES_EMBED))
#endif

#if DATABOX_USE_RANGE_CHECK
#define DATABOX_IS_INTEGER(box)                                                \
    databoxCheckRange(DATABOX_SIGNED_64, DATABOX_UNSIGNED_64, (box)->type)
#else
#define DATABOX_IS_INTEGER(box)                                                \
    ((box)->type == DATABOX_SIGNED_64 || (box)->type == DATABOX_UNSIGNED_64)
#endif

#define DATABOX_IS_SIGNED_INTEGER(box) ((box)->type == DATABOX_SIGNED_64)

#define DATABOX_IS_UNSIGNED_INTEGER(box) ((box)->type == DATABOX_UNSIGNED_64)

#if DATABOX_USE_RANGE_CHECK
#define DATABOX_IS_FLOAT(box)                                                  \
    databoxCheckRange(DATABOX_FLOAT_32, DATABOX_DOUBLE_64, (box)->type)
#else
#define DATABOX_IS_FLOAT(box)                                                  \
    ((box)->type == DATABOX_FLOAT_32 || (box)->type == DATABOX_DOUBLE_64)
#endif

#if DATABOX_USE_RANGE_CHECK
#define DATABOX_IS_NUMERIC(box)                                                \
    databoxCheckRange(DATABOX_SIGNED_64, DATABOX_DOUBLE_64, (box)->type)
#else
#define DATABOX_IS_NUMERIC(box)                                                \
    (DATABOX_IS_INTEGER(box) || DATABOX_IS_FLOAT(box))
#endif

#define databoxLen(box) ((box)->len)

#define databoxBytes(box)                                                      \
    ({                                                                         \
        assert((box)->type != DATABOX_BYTES_OFFSET);                           \
        (DATABOX_IS_BYTES_EMBED(box) ? (box)->data.bytes.embed                 \
                                     : (box)->data.bytes.start);               \
    })

#define databoxBytesEmbed(box) (box)->data.bytes.embed

#define databoxCBytes(box)                                                     \
    (DATABOX_IS_BYTES_EMBED(box) ? (box)->data.bytes.cembed                    \
                                 : (box)->data.bytes.cstart)

#define databoxUpdateBytesAllowEmbed(box, ptr, len_)                           \
    do {                                                                       \
        if ((len_) <= sizeof((box)->data.bytes.embed)) {                       \
            (box)->type = DATABOX_BYTES_EMBED;                                 \
            memcpy((box)->data.bytes.embed, ptr, len_);                        \
        } else {                                                               \
            (box)->type = DATABOX_BYTES;                                       \
            (box)->data.bytes.custart = (ptr);                                 \
        }                                                                      \
        (box)->len = (len_);                                                   \
    } while (0)

/* Hand ownership of a heap-allocated buffer to the box.
 * databoxFreeData zfree's the buffer when the box is released.
 * Use this when you already have a heap pointer (snprintf
 * result, parser output) instead of hand-writing the
 * type/data.ptr/len/allocated four-field assignment. */
#define databoxUpdateBytesOwned(box, ptr_, len_)                               \
    do {                                                                       \
        (box)->type = DATABOX_BYTES;                                           \
        (box)->data.ptr = (ptr_);                                              \
        (box)->len = (len_);                                                   \
        (box)->allocated = true;                                               \
    } while (0)

#define databoxOffsetBoxToRealBox(box, base)                                   \
    do {                                                                       \
        assert((box)->type == DATABOX_BYTES_OFFSET);                           \
        databoxUpdateBytesAllowEmbed(                                          \
            box, ((uint8_t *)base) + (box)->data.bytes.offset, (box)->len);    \
    } while (0)

#define databoxUpdateOffsetAllowEmbed(box, ptr, offset, len)                   \
    do {                                                                       \
        if ((len) <= sizeof((box)->data.bytes.embed)) {                        \
            /* No point in storing an offset if we can embed directly. */      \
            (box)->type = DATABOX_BYTES_EMBED;                                 \
            memcpy((box)->data.bytes.embed, (const uint8_t *)(ptr) + (offset), \
                   len);                                                       \
        } else {                                                               \
            (box)->type = DATABOX_BYTES_OFFSET;                                \
            (box)->data.bytes.offset = (offset);                               \
        }                                                                      \
        (box)->len = len;                                                      \
    } while (0)

#define databoxToIovec(box, iov)                                               \
    do {                                                                       \
        (iov)->iov_base = (void *)databoxBytes(box);                           \
        (iov)->iov_len = (box)->len;                                           \
    } while (0)

extern const databox DATABOX_BOX_TRUE;
extern const databox DATABOX_BOX_FALSE;
extern const databox DATABOX_BOX_NULL;
extern const databox DATABOX_BOX_VOID;

bool databoxGetBytes(databox *box, uint8_t **buf, size_t *len);
bool databoxGetSize(const databox *box, size_t *len);

/* The bytes of 'box''s value for every type: databoxGetSize's size, 16 for
 * a 96/128-bit integer, a pointer's width, and 0 for a type with no
 * payload (markers, true/false/null, void) */
size_t databoxGetSizeMinimum(const databox *box);
void databoxCopyBytes(databox *box, const void *ptr, size_t len);
databox databoxNewBytes(const void *ptr, size_t len);
databox databoxNewBytesString(const char *str);
databox databoxNewBytesAllowEmbed(const void *ptr, size_t len);
databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len);
databox databoxNewOffsetAllowEmbed(const void *ptr, size_t offset, size_t len);
databox databoxNewReal(double value);
/* Value equality, following IEEE for reals: NaN equals nothing, not even
 * itself, and -0.0 equals +0.0.  Otherwise the same as databoxCompare == 0. */
bool databoxEqual(const databox *a, const databox *b);

/* Total order for sorting and for container key identity (multimap,
 * multidict and the rest find keys with compare == 0).  Deterministic on every
 * platform: NaN sorts above +inf, and every NaN is the same position, so a
 * NaN key can be found and deleted again.  Use databoxEqual, not
 * compare == 0, when a NaN must not equal itself. */
int databoxCompare(const databox *a, const databox *b);

/* A 64-bit key for hashing a number by value, so that boxes databoxCompare
 * calls equal get equal keys whatever their types.  For a numeric box
 * (SIGNED_64, UNSIGNED_64, 96/128-bit with a payload, FLOAT_32, DOUBLE_64,
 * and PTR and CONTAINER_REFERENCE_EXTERNAL, which join the unsigned order)
 * it writes *key and returns true; any other box returns false, and the
 * caller hashes its bytes instead.
 *   - An integer value in the int64 range keys as its int64 bits, in any
 *     box: UNSIGNED_64 5, SIGNED_128 5, 5.0f and 5.0 all key as 5.
 *   - Other integer values, up to the 128-bit range, key as a mix of their
 *     sign and magnitude, in any box (integral reals included).
 *   - -0.0 keys as 0, and every NaN, of any sign or payload, has one key.
 *   - Any other real keys as a mix of its double bits (a float widens).
 * Unequal values may share a key: it is a hash input, not an identity.
 * Aborts on a deferred 96/128-bit box (no payload). */
bool databoxNumericHashKey(const databox *box, uint64_t *key);

/* Canonical numerals.
 *
 * datakit stores a byte string that spells a number as that number, in its
 * minimum representation, and readers interpret.  These two functions are
 * the one home of that rule; flexInsertBytes and every container boundary
 * use them.
 *
 * databoxCanonicalNumeral writes the canonical form of 'in' to *out and
 * returns true when 'in' is a BYTES or BYTES_EMBED box holding a numeral:
 *   - content StrScanScanReliable accepts becomes its SIGNED_64,
 *     UNSIGNED_64, FLOAT_32 or DOUBLE_64 box (big = 0);
 *   - an integer numeral beyond 64 bits, down to INT128_MIN and up to
 *     UINT128_MAX, with the same syntax (no sign but a leading '-', no
 *     leading zeros), becomes a SIGNED_128 box when negative and an
 *     UNSIGNED_128 box when positive.  Its payload is stored in out->extra
 *     (big = 1, allocated = 0).  StrScanScanReliableConvert128 parses it.
 * Anything else returns false and leaves *out untouched, and the caller keeps
 * 'in' as it is.  Boxes of any other type, numbers included, return false.
 *
 * It never allocates.  A 128-bit result points into 'out' itself, so it is
 * valid for as long as 'out' is and must not be copied by value; use
 * databoxBigCopyFrom to move it.  'in' is only read: the result does not
 * borrow from its bytes. */
bool databoxCanonicalNumeral(const databox *in, databoxBig *out);

/* An integral real as the integer it equals: true, with *out the SIGNED_64
 * (negative) or UNSIGNED_64 box, for a FLOAT_32 or DOUBLE_64 holding a whole
 * number in [-2^63, 2^64).  False (and *out untouched) for any other box,
 * fractions, infinities, NaN, values beyond that range, and -0.0 (its sign
 * is kept).  'in' and 'out' may be the same box. */
bool databoxIntegralRealAsInteger(const databox *in, databox *out);

/* The box to store or compare with on the minimal-storage (auto-convert)
 * path: 'in' itself, or 'scratch' holding its canonical form.  A numeral
 * string becomes its number (databoxCanonicalNumeral; "5.0" stays the real
 * 5.0, so it prints back as given), and a real box holding a whole number
 * becomes that integer (databoxIntegralRealAsInteger): 5.0 stores as 5.  The
 * inline
 * fast reject returns 'in' with no call for boxes that are neither reals
 * nor BYTES / BYTES_EMBED, for empty strings, and for strings whose first
 * byte is neither '-' nor a digit, because no numeral starts any other way.
 * 'scratch' is written only when the result is 'scratch'. */
static inline const databox *databoxCanonical(const databox *in,
                                              databoxBig *scratch) {
    if (in->type == DATABOX_FLOAT_32 || in->type == DATABOX_DOUBLE_64) {
        return databoxIntegralRealAsInteger(in, (databox *)scratch)
                   ? (const databox *)scratch
                   : in;
    }

    if (!DATABOX_IS_BYTES(in) || in->len == 0) {
        return in;
    }

    const uint8_t first = DATABOX_IS_BYTES_EMBED(in)
                              ? in->data.bytes.embed[0]
                              : in->data.bytes.custart[0];
    if (first != '-' && (uint8_t)(first - '0') > 9) {
        return in;
    }

    /* A numeral keeps its spelling: "22.0" parses to the real 22.0, which
     * prints back as "22.0"; only a real given as a number becomes an
     * integer */
    return databoxCanonicalNumeral(in, scratch) ? (const databox *)scratch : in;
}

/* Trivial constructors are inline: they appear on hot paths (boxing
 * entries for flex/multimap inserts) where a function call per element
 * would dominate the work. */
static inline databox databoxNewUnsigned(uint64_t val) {
    return (databox){.type = DATABOX_UNSIGNED_64, .data.u = val};
}

static inline databox databoxNewSigned(int64_t val) {
    return (databox){.type = DATABOX_SIGNED_64, .data.i = val};
}

static inline databox databoxNewPtr(void *ptr) {
    return (databox){.type = DATABOX_PTR, .data.ptr = ptr};
}

/* Box a pointer as DATABOX_UNSIGNED_64 instead of DATABOX_PTR.
 * Use this when the pointer is an element of a SORTED container entry
 * (multimap/flex sorted inserts): DATABOX_PTR sits outside the numeric
 * compare classes, so databoxCompare orders it by type tag rather than
 * by value against the DATABOX_UNSIGNED_64 that flex storage decodes
 * back to. Boxing as unsigned keeps every element of an entry in one
 * compare class. (In-memory use only: a stored pointer is meaningless
 * once persisted.) */
static inline databox databoxNewPtrAsUnsigned(const void *ptr) {
    return (databox){.type = DATABOX_UNSIGNED_64, .data.u = (uintptr_t)ptr};
}

/* Retrieve a pointer stored via databoxNewPtrAsUnsigned() (also correct
 * for such entries decoded back out of flex/multimap storage, which
 * return them as DATABOX_UNSIGNED_64) */
static inline void *databoxRetrievePtrUnsigned(const databox *box) {
    return (void *)(uintptr_t)box->data.u64;
}

/* clang-format off */
/* clang-format on */

databox databoxBool(bool which);
databox databoxNull(void);
databox databoxVoid(void);

/* Make BYTES box 'dst' own its value: copied into 'bytes' (heap memory of
 * at least dst->len bytes, handed to the box and freed with it), or, when
 * short enough, embedded in the box with 'bytes' freed at once */
void databoxRetainBytesSelfExact(databox *dst, void *bytes);
ssize_t databoxRetainBytesSelf(databox *dst, databoxRetainCache *cache);
/* Owning copies.  databoxCopy returns, and databoxCopyBytesFromBox writes
 * to 'dst', a copy that is independent of the source's storage;
 * databoxAllocateIfNeeded makes 'box' independent in place.  Plain bytes
 * (BYTES, BYTES_NEVER_FREE) are copied to the heap as owned BYTES, or
 * embedded when they fit 8 bytes; an embedded flex aggregate is copied to
 * the heap under its own type; an mdsc string (PTR_MDSC) becomes a new
 * mdsc; a 96/128-bit payload is copied to a 16-byte heap block with
 * allocated = 1 and big = 1.  Release any of them with databoxFreeData.
 * BYTES_VOID and BYTES_OFFSET carry no bytes of their own and stay as they
 * are.  Every other result has big = 0, and a type-only wide box
 * (data.ptr == NULL) stays type-only.  databoxAllocateIfNeeded leaves an
 * already-owned box alone. */
void databoxCopyBytesFromBox(databox *dst, const databox *src);
bool databoxAllocateIfNeeded(databox *box);
databox databoxCopy(const databox *src);

/* Re-homing copy: copy 'src' into 'dst' without allocating.  A 96/128-bit
 * payload is copied into dst->extra and data.i128 / data.u128 is aimed
 * there, with big = 1; any other value gets big = 0.  'dst' never owns
 * anything (allocated = 0): BYTES still borrow from wherever 'src' points,
 * and 'src' keeps whatever it owned.  'src' may alias 'dst'.  Use this to
 * keep a wide value read from storage that is about to go away, such as
 * a library-filled entry. */
void databoxBigCopyFrom(databoxBig *dst, const databox *src);

/* Releases what the box owns: allocated BYTES, an allocated 96/128-bit
 * heap payload, or an allocated MDSC pointer; the box becomes VOID.  A
 * databoxBig whose payload pointer is aimed at its own 'extra' owns
 * nothing and is left as is. */
void databoxFreeData(databox *box);
void databoxFree(databox *box);

void databoxRepr(const databox *box);
const char *databoxReprStr(const databox *const box);
void databoxReprSay(const char *msg, const databox *box);

#ifdef DATAKIT_TEST
int databoxTest(int argc, char *argv[]);
#endif
