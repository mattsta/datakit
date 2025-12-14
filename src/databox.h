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

/* Macro-ize common databox metadata because we must share _exactly_ the same
 * layout and fields between 'databox' and 'databoxBig' */
#define databoxInternals_                                                      \
    uint64_t type : 8;      /* databoxtype; valid types start at offset 1 */   \
    uint64_t allocated : 1; /* bool; true if need to free data.bytes.start */  \
    uint64_t created : 1;   /* bool; if retrieved+created, true. */            \
    uint64_t big : 1;       /* bool; true if actually a databoxBig */          \
    uint64_t unused : 5;                                                       \
    uint64_t len : 48 /* length of 'data.bytes'. 281 TB should be enough. */

/* databox is a 8 + 8 = 16 byte struct. */
typedef struct databox {
    databoxUnion data; /* 8 bytes */
    databoxInternals_;
} databox;
#if DK_C11
_Static_assert(sizeof(databox) == 16, "databox struct too big!");
#endif

/* 'databoxBig' is 32 bytes total */
typedef struct databoxBig {
    databoxUnion data;                  /* 8 bytes */
    databoxInternals_;                  /* 8 bytes */
    uint8_t extra[sizeof(__uint128_t)]; /* 16 bytes */
} databoxBig;
#if DK_C11
_Static_assert(sizeof(__int128_t) == sizeof(__uint128_t),
               "Your 128 bit integers are weird?");
_Static_assert(sizeof(databoxBig) == 32, "databoxBig struct too big!");
#endif

#define DATABOX_BIG_INIT(dbig)                                                 \
    do {                                                                       \
        (dbig)->big = true;                                                    \
        (dbig)->data.u128 = (__uint128_t *)(dbig)->extra;                      \
    } while (0)

/* Growable databox where databoxStr.box.data.ptr == databoxStr.bytes */
typedef struct databoxStr {
    databox box;
    uint8_t bytes[];
} databoxStr;

#define DATABOX_SIZE(box) (((box)->type >= DATABOX_BYTES) ? (box)->len : 8)

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

#define DATABOX_NAN                                                            \
    (databox) {                                                                \
        .data.d64 = DK_NAN_64, .type = DATABOX_DOUBLE_64                       \
    }

#define DATABOX_INFINITY_POSITIVE                                              \
    (databox) {                                                                \
        .data.d64 = DK_INFINITY_POSITIVE_64, .type = DATABOX_DOUBLE_64         \
    }

#define DATABOX_INFINITY_NEGATIVE                                              \
    (databox) {                                                                \
        .data.d64 = DK_INFINITY_NEGATIVE_64, .type = DATABOX_DOUBLE_64         \
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
        (box)->data.d64 = DK_NAN_64;                                           \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_INFINITY_POSITIVE(box)                                     \
    do {                                                                       \
        (box)->data.d64 = DK_INFINITY_POSITIVE_64;                             \
        (box)->type = DATABOX_DOUBLE_64;                                       \
    } while (0)

#define DATABOX_SET_INFINITY_NEGATIVE(box)                                     \
    do {                                                                       \
        (box)->data.d64 = DK_INFINITY_NEGATIVE_64;                             \
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
size_t databoxGetSizeMinimum(const databox *box);
databox databoxNewBytes(const void *ptr, size_t len);
databox databoxNewBytesString(const char *str);
databox databoxNewBytesAllowEmbed(const void *ptr, size_t len);
databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len);
databox databoxNewOffsetAllowEmbed(const void *ptr, size_t offset, size_t len);
databox databoxNewUnsigned(uint64_t val);
databox databoxNewSigned(int64_t val);
databox databoxNewReal(double value);
databox databoxNewPtr(void *ptr);
bool databoxEqual(const databox *a, const databox *b);
int databoxCompare(const databox *a, const databox *b);

/* clang-format off */
/* clang-format on */

databox databoxBool(bool which);
databox databoxNull(void);
databox databoxVoid(void);

void databoxRetainBytesSelfExact(databox *dst, void *bytes);
ssize_t databoxRetainBytesSelf(databox *dst, databoxRetainCache *cache);
void databoxCopyBytesFromBox(databox *dst, const databox *src);
bool databoxAllocateIfNeeded(databox *box);
databox databoxCopy(const databox *src);
void databoxFreeData(databox *box);
void databoxFree(databox *box);

void databoxRepr(const databox *box);
const char *databoxReprStr(const databox *const box);
void databoxReprSay(const char *msg, const databox *box);

#ifdef DATAKIT_TEST
int databoxTest(int argc, char *argv[]);
#endif
