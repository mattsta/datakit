/* flex — a pointer-free multi-type storage container
 *
 * Copyright 2016-2020 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* ====================================================================
 *
 * FLEX LAYOUT:
 * <fbytes><fcount><entry><entry>
 *
 * <fbytes> is one to nine bytes of an unsigned variable length integer
 * holding the total size of the flex.
 * (Physical encoding: split varint)
 *
 * <fcount> is one to nine bytes of an unsigned variable length integer
 * holding the count of elements in the flex.
 * (Physical encoding: tagged varint)
 *
 * FLEX ENTRIES:
 * <encoding><data><encoding reversed>
 *
 * A regular flex entry has three fields:
 *   1. encoding type of the data with either explicit or implicit length
 *   2. the data itself
 *   3. metadata again, but backwards so we can traverse the list in reverse
 *
 * A special immediate value flex entry (true, false, null) only has:
 *   1. encoding byte
 *
 * Encodings overview:
 * We have three encoding types, but over 40 individual encodings.
 *
 * The first two encoding types are for byte strings and store lengths.
 * The third encoding type is for non-string data including immediate
 * values, integers, floats (16, 32, 64), and other useful types where
 * we know the full length of the data just from the type details.
 *
 * Encodings of the first type (type byte begins with 00); strings only
 * ------------------------------------------------------
 * |00xxxxxx| - Length up to 64 bytes (6 bits).
 *              We use a NoZero encoding here to allow 64 bytes instead of
 *              63 bytes since we don't allow zero-byte quantities to be
 *              stored using these encodings.
 *              Length is encoded in the type byte itself.
 * |01xxxxxx|yyyyyyyy| - Length up to 16447 bytes (14 bits + 64 from previous).
 *                       Length is encoded in type byte + one extra byte.
 * These encodings are special because they contain length information *inside*
 * the type byte itself, which lets us store small data with small overhead.
 *
 * Encodings of the second type (type byte begins with 10); strings only
 * -------------------------------------------------------
 * |1000000| to |10001001| - Length up 2^64 - 1.
 * These lengths are encoded as an external varint after the type byte.
 *
 * Encodings of the third type (type byte begins with 11); all other types
 * ------------------------------------------------------
 * |11xxxxxx| - 1 byte
 *      Fixed value encodings including integers, floats, true/false, null, etc.
 *      For these types, we know the data size based on the encoding, so
 *      we can infer the total data length just by one type byte.
 * ==================================================================== */

#define DK_TEST_VERBOSE 0

#if DK_TEST_VERBOSE
#include <inttypes.h>
#include <stdio.h>
#endif

#include "../deps/varint/src/varintExternal.h"        /* user integers */
#include "../deps/varint/src/varintSplitFullNoZero.h" /* byte lengths */
#include "../deps/varint/src/varintTagged.h"          /* count */
#include "datakit.h"
#include "flex.h"

#include "cleaner.h"
#include "str.h"

#include "multimapAtom.h"

#include "conformLittleEndian.h"
#include "util.h"
#include <assert.h>

#include "float16.h"

/* Scope-exit free for heap spills (wide-entry insert scratch) */
defcleaner(void *, zfree);
defcleanerName(flex **, zfree, FlexPtrs);
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(int *, zfree, Ints);
defcleanerName(int64_t *, zfree, I64);
defcleanerName(char *, zfree, Chars);
defcleaner(flex *, flexFree);
#ifdef DATAKIT_TEST
defcleaner(multimapAtom *, multimapAtomFree);
#endif
#include <math.h> /* isfinite (container math) */

/* This configuration define allows building flex without 'mdsc', but disabling
 * MDSC support does NOT remove the MDSC types. It only removes the ability for
 * flex to consume MDSC inputs as external references. */
#ifndef FLEX_ENABLE_PTR_MDSC
#define FLEX_ENABLE_PTR_MDSC 1
#endif

#if FLEX_ENABLE_PTR_MDSC
#include "mdsc.h"
#ifdef DATAKIT_TEST
defcleaner(mdsc *, mdscfree);
#endif
#endif

/* Toggle for enabling performance-damaging assert() locations */
#if 0
#define FLEX_DEBUG_EXTENSIVE
#endif

/* Toggle to undo mandatory inline for testing */
#if 0
#ifdef DK_INLINE_ALWAYS
#undef DK_INLINE_ALWAYS
#define DK_INLINE_ALWAYS static inline
#endif
#endif

/* Note: we could potentially start at (VARINT_SPLIT_FULL_NO_ZERO_BYTE_6 + 1)
 * because BYTE_7 is 281 terabytes and we probably can't grow that large.
 * It would give us three more type IDs to use. */
#define FIRST_AVAILABLE_TYPE_BYTE                                              \
    (VARINT_SPLIT_FULL_NO_ZERO_BYTE_8 + 1) /* VALUE: 11001001 */

/* ====================================================================
 * Definitions for fixed types
 * ==================================================================== */
#define FLEX_FIXED_START FIRST_AVAILABLE_TYPE_BYTE

/* These types describe the physical in-memory and on-disk flex format.
 * Any addition, removal, or re-ordering of type values must result in
 * incrementing a flex version number. */
typedef enum flexType {
    /* 16 integer types
     *  (16 = 8*2 = 8 size classes * (signed vs. unsigned)) */
    /* Note: these orders matter because we determine "negative" storage
     * types by subtracting one from the unsigned type encoding. */
    FLEX_VARINT_SPLIT_MAX = VARINT_SPLIT_FULL_NO_ZERO_BYTE_8,

    /************ BEGIN INTEGER TYPES ************/
    FLEX_NEG_8B = FLEX_FIXED_START, /* (FLEX_FIXED_START + 0) == 201 */
    FLEX_UINT_8B,
    FLEX_NEG_16B,
    FLEX_UINT_16B,
    FLEX_NEG_24B,
    FLEX_UINT_24B, /* (FLEX_FIXED_START + 5) */
    FLEX_NEG_32B,
    FLEX_UINT_32B,
    FLEX_NEG_40B,
    FLEX_UINT_40B,
    FLEX_NEG_48B, /* (FLEX_FIXED_START + 10) */
    FLEX_UINT_48B,
    FLEX_NEG_56B,
    FLEX_UINT_56B,
    FLEX_NEG_64B,
    FLEX_UINT_64B, /* (FLEX_FIXED_START + 15) */
    /* SKIP 72, 80, 88 */
    FLEX_NEG_96B,
    FLEX_UINT_96B,
    /* SKIP 104, 112, 120 */
    FLEX_NEG_128B,
    FLEX_UINT_128B, /* (FLEX_FIXED_START + 19) */

    /************ BEGIN FLOAT TYPES ************/
    FLEX_REAL_B16B, /* Truncated float32 (Google's bfloat16 format) */
    FLEX_REAL_16B,  /* float16 (IEEE 754-2008 binary16) */
    FLEX_REAL_32B,  /* float32 (IEEE 754-2008 binary32) */
    FLEX_REAL_64B,  /* float64 (IEEE 754-2008 binary64) */

    /* TODO */
    FLEX_REAL_32D, /* IEEE 32 bit float decimal */
    FLEX_REAL_64D, /* IEEE 64 bit float decimal */

    /************ BEGIN EXTERNAL PHYSICAL REFERENCE TYPES ************/
    /* To avoid duplicating key bytes when using indexes, we allow values to
     * be mdsc entries.
     *
     * We define two storage types.
     * If we can remove the top 16 bits of a pointer, we save 2 bytes and
     * store as FLEX_EXTERNAL_MDSC_48B. If we can't remove the top 2 bytes,
     * we store FLEX_EXTERNAL_MDSC_64B.
     *
     * The benefit of this type is:
     *  - maps are still sorted by the value of what's pointed to, not the
     * value of the pointer itself.
     *      - BUT NOTE: values must NOT BE CHANGED externally.
     *      - if changing value is required, it must be deleted from map
     * BEFORE the change
     *      - then re-inserted AFTER the change
     *      - if values are changed by pointer access after insert,
     * subsequent find behavior won't be able to locate the key (assuming it
     * is a key (if your entry is a non-key entry, you can change it
     * whenever and however you like externally). */
    FLEX_EXTERNAL_MDSC_48B,
    FLEX_EXTERNAL_MDSC_64B,

    /************ BEGIN LOGICAL REFERENCE TYPES ************/
    /* 8 reference types.
     *   value following external reference is a logical pointer to some
     * other data maintained by the user. We also use these to represent
     * stored pointers, but it's up to the user to know when a restored
     * databox reference is a pointer versus an actual user-level reference.
     */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_8,  /* up to 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_16, /* up to 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_24, /* up to 16 million + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_32, /* up to 4B + 16M + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_40, /* to 1T + 4B + 16M + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_48, /* to 281T + 1T + 4B + 16M + ...
                                           */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_56, /* to 72PB + 281T + ... */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_64, /* to 18EB + ... */

    /************ BEGIN BOX MARKER TYPES ************/
    /* Note: the order of the next 8 types matter.
     *       The order here must match the order in the databox type enum.
     */
    /* 4 container holder types.
     *   value is a flex with conforming layout for type specified. */
    FLEX_CONTAINER_MAP, /* map is our embedded K/V pair type */
    FLEX_CONTAINER_LIST,
    FLEX_CONTAINER_SET,
    FLEX_CONTAINER_TUPLE,

    /* Compress variants; do we need this many? Can we combine
     * these? Should we make this a 2-level type with:
     * [FLEX_CONTAINER][CONTAINER_TYPE][data][EPYT][RENIATNOC] */
    FLEX_CONTAINER_CMAP, /* map is our embedded K/V pair type */
    FLEX_CONTAINER_CLIST,
    FLEX_CONTAINER_CSET,
    FLEX_CONTAINER_CTUPLE,

    /* NOTE: If you add any types between here and FLEX_SAME, you
     *       need to update the first test case accounting for
     *       type differences. */

    /************ BEGIN IMMEDIATE TYPES ************/
    /* 1 marker to mean "current value is same as previous value."
     *   Allows us to avoid storing duplicate entries.
     *   If the previous entry matches the current entry, we
     *   just store a one byte FLEX_SAME marker instead. */
    FLEX_SAME = 251, /* UNUSED */

    /* 4 immediate value types; our highest type values */
    FLEX_BYTES_EMPTY = 252, /* empty, since we can't store 0 len directly. */
    FLEX_TRUE = 253,
    FLEX_FALSE = 254,
    FLEX_NULL = 255 /* FINAL VALUE.  SET NO MORE. MAX IS 255 */
} flexType;

/* Determine proper encoding for string
 * ====================================
 * We do not support zero length byte storage, but we do have a special
 * type for "this is bytes, but there are no bytes here" (FLEX_BYTES_EMPTY).
 *
 * **BUT** Adding this check during every insert has 2x worst case performance
 * due to the extra cmov instead of just setting the string type to always zero.
 *
 * If you *KNOW* you will never try to store zero-length byte entries in a flex,
 * define FLEX_ENABLE_STRING_DANGEROUS=1 to get a minor speed improvement.
 *
 * If you do attempt to store a byte string with length 0 in STRING_DANGEROUS
 * mode, you'll see flexRepr() report a {{NULL}} instead of {{EMPTY}} and your
 * flex sizes will be too big by two bytes and your tail pointer most likely
 * won't be in the right place.
 */
#if FLEX_ENABLE_STRING_DANGEROUS
#define FLEX_STRING_ENCODING(len) (0)
#else
#define FLEX_STRING_ENCODING(len) (unlikely((len) == 0) ? FLEX_BYTES_EMPTY : 0)
#endif

/* Integer encoding step is the number of elements between successive
 * same-signed integer encodings.
 * (which is 2 since the steps are (negative, unsigned),
 * but this is more programatically explicit) */
#define FLEX_INTEGER_ENCODING_STEP (FLEX_UINT_16B - FLEX_UINT_8B)

/* Width (as a varintWidth) of a fixed-int flex encoding.
 *
 * The <=64-bit integer encodings are CONTIGUOUS (NEG/UINT pairs, STEP apart),
 * so the position formula yields their width directly.  The big-int encodings
 * are NOT contiguous with them — flex jumps 64B -> 96B -> 128B with no
 * 72/80/88/104/112/120-bit encodings — so the bare formula under-counts them
 * (FLEX_UINT_128B would resolve to VARINT_WIDTH_80B, i.e. 10 bytes, and the
 * writer would fill only 10 of the 16 reserved bytes, corrupting the high
 * half).  Special-case the two big widths; the common <=64-bit path keeps its
 * single subtract+divide. */
#define EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding)                         \
    ((encoding) <= FLEX_UINT_64B                                               \
         ? ((((encoding) - FLEX_NEG_8B) / FLEX_INTEGER_ENCODING_STEP) + 1)     \
         : ((encoding) <= FLEX_UINT_96B ? VARINT_WIDTH_96B                     \
                                        : VARINT_WIDTH_128B))

/* + 1 because our minimum reference is a 1 byte integer */
#define EXTERNAL_VARINT_WIDTH_FROM_REFERENCE_(encoding)                        \
    (((encoding) - FLEX_CONTAINER_REFERENCE_EXTERNAL_8) + 1)

#define FLEX_CONTAINER_OFFSET(type) ((type) - FLEX_CONTAINER_MAP)

/* ====================================================================
 * Macros for accessing flex metadata contents
 * ==================================================================== */
/* Strings are encoded by their length.  All lengths have an initial byte
 * guaranteed to be <= FLEX_VARINT_SPLIT_MAX */
#define FLEX_IS_STR(enc) ((enc) <= FLEX_VARINT_SPLIT_MAX)

/* Immediate encodings are single-byte encodings (we don't duplicate
 * the encoding for reverse traversal because there's no inner "contents"
 * to skip over).
 * Immediate encodings are for any of:
 *   - one byte markers (SAME)
 *   - one byte values (T/F/N) */
#define FLEX_IS_IMMEDIATE(encoding) ((encoding) >= FLEX_SAME)

/* Integers are between types negative 8 bits and unsigned 64 bits. */
#define FLEX_IS_INTEGER(enc) ((enc) >= FLEX_NEG_8B && (enc) <= FLEX_UINT_64B)
#define FLEX_IS_INTEGER_BIG(enc)                                               \
    ((enc) >= FLEX_NEG_96B && (enc) <= FLEX_UINT_128B)
#define FLEX_IS_BOOL(enc) ((enc) == FLEX_TRUE || (enc) == FLEX_FALSE)

/* Identify an external refrence */
#define FLEX_IS_REF_EXTERNAL(enc)                                              \
    ((enc) >= FLEX_CONTAINER_REFERENCE_EXTERNAL_8 &&                           \
     (enc) <= FLEX_CONTAINER_REFERENCE_EXTERNAL_64)

/* Identify a type indicating a nested type we need to further consult */
#define FLEX_IS_FORWARD_DECLARE_SUBCONTAINER(enc)                              \
    ((enc) >= FLEX_CONTAINER_MAP && (enc) < FLEX_SAME)

/* Empty container header: 2 bytes (split varint) + 0 count (tagged varint) */
#define FLEX_EMPTY_HEADER_SIZE (sizeof(uint8_t) + sizeof(uint8_t))

/* Accessors */
#define FLEX_ENTRY_HEAD(f) (flexEntry *)((f) + FLEX_HEADER_SIZE(f))
#define FLEX_ENTRY_END(f) (flexEntry *)((f) + flexTotalBytes_(f))
#define FLEX_ENTRY_AFTER_TAIL(f) (FLEX_ENTRY_END(f))

#define IS_HEAD(fe, f) ((fe) == FLEX_ENTRY_HEAD(f))
#define GET_END(f) FLEX_ENTRY_END(f)

/* flexEncoding is flexType but with string values too... */
typedef uint_fast8_t flexEncoding;

typedef struct flexEntryData {
    const flexEntry *fe;
    size_t len;               /* length of this data after the header */
    varintWidth encodingSize; /* bytes required to store encoding/len */
    flexEncoding encoding;
} flexEntryData;

typedef struct flexInsertContents {
    /* populated by user */
    const void *data;
    size_t len;
    flexEncoding encoding;

    /* populated by internal functions for accounting during writing */
    uint_fast8_t encodingLen;

    /* Don't write anything, just allocate arbitrary space. */
    bool isVoidEntry;
} flexInsertContents;

/* Returns size of metadata encoding only.  Between 1 and 8 bytes.
 * If encoding is nested, the nested encoding is *not* included here,
 * the nested encoding is part of the data. */
DK_INLINE_ALWAYS uint_fast8_t
abstractEncodingSizeTotal(const flexEntry *fe, uint_fast8_t feEncodingSize) {
    const flexEncoding encoding = fe[0];

    /* Normal:
     *   [IMMEDIATE]
     *    - OR -
     *   [ENCODING][DATA][ENCODING] */
    return FLEX_IS_IMMEDIATE(encoding) ? 1 : feEncodingSize * 2;
}

#define FLEX_ENCODING_SIZE_TOTAL_FORWARD(fe, encsize)                          \
    abstractEncodingSizeTotal(fe, encsize)

#define FLEX_ENCODING_SIZE_TOTAL_REVERSE(fe, encsize)                          \
    abstractEncodingSizeTotal(fe, encsize)

#define FLEX_ENCODING_SIZE_TOTAL_FROM_ENCODING(enc, encSize)                   \
    abstractEncodingSizeTotal(&(enc), encSize)

typedef uint_fast8_t flexData;
/* Entry Layout:
 *   [LEN][DATA][LEN REVERSED] */
#define FLEX_ENTRY_DATA(fed) ((flexData *)((fed)->fe + (fed)->encodingSize))
#define FLEX_ENTRY_META_SIZE(fed)                                              \
    (FLEX_ENCODING_SIZE_TOTAL_FORWARD((fed)->fe, (fed)->encodingSize))

/* Total size is: meta (header + trailer) + length of data */
#define FLEX_ENTRY_SIZE_TOTAL(fed)                                             \
    (size_t)(FLEX_ENTRY_META_SIZE(fed) + (fed)->len)

DK_INLINE_ALWAYS size_t flexDataSizeForFixedWidthEncodingWithInnerEntry(
    const flexEncoding encoding, const flexEntry *innerFe,
    varintWidth *encodingSize, const bool isForward);

DK_INLINE_ALWAYS void abstractGetLength(const flexEntry *restrict fe,
                                        varintWidth *restrict lensize,
                                        size_t *restrict len,
                                        const bool isForward) {
    const uint_fast8_t encoding = fe[0];
    if (FLEX_IS_STR(encoding)) {
        if (isForward) {
            varintSplitFullNoZeroGet_(fe, *lensize, *len);
        } else {
            varintSplitFullNoZeroReversedGet_(fe, *lensize, *len);
        }
    } else {
        *len = flexDataSizeForFixedWidthEncodingWithInnerEntry(
            encoding, isForward ? fe + 1 : fe - 1, lensize, isForward);
    }
}

/* Get length of element when walking backwards.
 * 'fe' points to the reverse type byte of an entry. */
#define FLEX_DECODE_LENGTH_REVERSE(fe_, prevlensize, prevlen)                  \
    abstractGetLength(fe_, &(prevlensize), &(prevlen), false)

#define FLEX_DECODE_LENGTH_FORWARD(fe_, lensize, len)                          \
    abstractGetLength(fe_, &(lensize), &(len), true)

#define FLEX_ENTRY_NEXT_(fe, len, lensize)                                     \
    ((fe) + ((len) + FLEX_ENCODING_SIZE_TOTAL_FORWARD(fe, lensize)))

#define FLEX_ENTRY_PREVIOUS_(fe, prevlen, prevEncoding, prevEncodingSize)      \
    ((fe) - (FLEX_IS_IMMEDIATE(prevEncoding)                                   \
                 ? 1                                                           \
                 : ((prevlen) + ((prevEncodingSize) * 2))))

DK_INLINE_ALWAYS flexEntry *flexGetPreviousEntry(flexEntry *const fe) {
    /* temp vars for intermediate calculations in the macros */
    varintWidth reverseByteCount;
    size_t reverseLength;

    assert(fe && (fe - 1)); /* quiet scan-build thinking 'fe-1' could be NULL */

    /* Note: 'flexGetPreviousEntry' is an unsafe version of 'flexPrev'.
     *       If 'fe' is the head element and we try to get the previous,
     *       we'll segfault. */
    FLEX_DECODE_LENGTH_REVERSE(fe - 1, reverseByteCount, reverseLength);
    return FLEX_ENTRY_PREVIOUS_(fe, reverseLength, (fe - 1)[0],
                                reverseByteCount);
}

/* Quickly check if accessing an endpoint.  If so, get endpoint directly. */
#define FLEX_INDEX(f, index, fe)                                               \
    do {                                                                       \
        /* Short circuit common endpoints */                                   \
        if ((index) == FLEX_ENDPOINT_HEAD) {                                   \
            (fe) = flexHead(f);                                                \
        } else if ((index) == FLEX_ENDPOINT_TAIL) {                            \
            (fe) = flexTail(f);                                                \
        } else {                                                               \
            (fe) = flexIndex(f, index);                                        \
        }                                                                      \
    } while (0)

/* ====================================================================
 * flex metadata control
 * ==================================================================== */
/* Layout is:
 *   [TOTAL BYTES...][COUNT...][ENTRY...] */
#define FLEX_TOTAL_BYTES_START_(f) (f)
#define FLEX_TOTAL_BYTES_WIDTH(f)                                              \
    (varintWidth)(varintSplitFullNoZeroGetLenQuick_(FLEX_TOTAL_BYTES_START_(f)))

#define FLEX_LENGTH_OF_ENCODING(fe)                                            \
    (FLEX_IS_STR((fe)[0]) ? varintSplitFullNoZeroGetLenQuick_(fe) : 1)

#define flexTailOffset_(f) (size_t)(flexTail(f) - (f))

#define FLEX_COUNT_START_(f) ((f) + FLEX_TOTAL_BYTES_WIDTH(f))
#define FLEX_COUNT_WIDTH(f)                                                    \
    (varintWidth)(varintTaggedGetLenQuick_(FLEX_COUNT_START_(f)))

#if 0
#define FLEX_COUNT_WIDTH_WITH_BYTES(f, bytesStart)                             \
    (varintWidth)(varintTaggedGetLenQuick_(f + bytesStart))
#endif

DK_STATIC size_t flexTotalBytes_(const flex *const f) {
    varintWidth size;
    size_t l;
    varintSplitFullNoZeroGet_(f, size, l);
    return l;
}

#define flexCount_(f) (ssize_t)(varintTaggedGet64Quick_(FLEX_COUNT_START_(f)))

#define FLEX_HEADER_SIZE(f)                                                    \
    (uint_fast8_t)(FLEX_TOTAL_BYTES_WIDTH(f) + FLEX_COUNT_WIDTH(f))

/* Returns the first forward type byte of the last entry in the flex.
 * How?  It uses the *last* type byte of the entry to:
 *   - Read the type of the element
 *   - Read the size of the element
 *   - Jump back N bytes and give us the last element in the flex. */
flexEntry DK_FN_PURE *flexTail(const flex *const f) {
    flexEntry *end = FLEX_ENTRY_END(f);

    if (IS_HEAD(end, f)) {
        return end;
    }

    const flexEntry *tailType = end - 1;

    D("[%p]: asking for tail with tailType %p [%d] (offset: %d); end:"
      "%p (offset: %d)\n",
      f, tailType, *tailType, (int)(tailType - f), end, (int)(end - f));

    size_t tailElementLength;
    varintWidth encodingSize;
    FLEX_DECODE_LENGTH_REVERSE(tailType, encodingSize, tailElementLength);

    /* return pointer to last element in flex, which is:
     *      END
     *          minus LENGTH OF DATA for last entry
     *          minus ENCODING SIZE (reverse encoding)
     *          minus ANOTHER ENCODING SIZE (forward encoding) */
    flexEntry *tail = end - tailElementLength -
                      FLEX_ENCODING_SIZE_TOTAL_REVERSE(tailType, encodingSize);

    D("pointing to tail of type %d from tail type of type %d\n", *tail,
      *tailType);

    assert(tail[0] == tailType[0]);
    return tail;
}

flexEntry DK_FN_PURE *flexTailWithElements(const flex *const f,
                                           uint_fast32_t elementsPerEntry) {
    flexEntry *tail = flexTail(f);

    while (--elementsPerEntry) {
        tail = flexGetPreviousEntry(tail);
    }

    return tail;
}

#define flexSetTotalBytesCount(f, nb, nc)                                      \
    flexSetTotalBytesCount_((f), (nb), (nc), false)

/* This is an optimization for inline size determination.  None of the split
 * varint macros can do all of this without creating additional temp vars. */
DK_INLINE_ALWAYS DK_FN_CONST varintWidth
flexVarintSplitFullNoZeroLenEmbedded_(size_t len) {
    if (len <= VARINT_SPLIT_FULL_NO_ZERO_STORAGE_1) {
        return 1 + 0;
    }

    if (len <= VARINT_SPLIT_FULL_NO_ZERO_STORAGE_2) {
        return 1 + 1;
    }

    if (len <= VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3) {
        return 1 + 2;
    }

    len -= VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3;
    varintWidth width = VARINT_WIDTH_8B;
#if 0
    while ((len >>= 8) != 0) {
        width++;
    }
#else
    if (width) {
        width = DK_BYTESUSED(len);
    }
#endif

    if (width == 1) {
        /* 1 + 2 == (encoding) + ((monotonic pad) + (width)) */
        return 1 + 2;
    }

    return 1 + width;
}

/* This function gets called for every insert and delete into the flex
 * since every insert and delete adjusts both the byte length of the flex
 * and the count of elements in the flex. */
DK_STATIC int_fast8_t flexSetTotalBytesCount_(flex **const _f,
                                              const size_t newBytes,
                                              const size_t newCount,
                                              const bool drain) {
    flex *f = *_f;

    /* Step 1: Establish preconditions */
    /* Step 1a: Get current widths of totalBytes and totalCount */
    const int_fast8_t currentTotalBytesWidth = FLEX_TOTAL_BYTES_WIDTH(f);
    const int_fast8_t currentCountWidth = FLEX_COUNT_WIDTH(f);

    /* Step 1b: Discover width of newCount */
    const int_fast8_t newCountWidth = varintTaggedLenQuick(newCount);

    /* Step 1c: Calculate width of new totalBytes including newCountWidth but
     *          *not* including TotalBytesWidth (yet).
     *          Note: this implicitly includes 'currentTotalBytesWidth' and
     *          we assume TotalBytesWidth isn't changing until we reach the
     *          while(bytesStorageDiff) loop below. */
    ssize_t newTotalBytes = newCountWidth + (newBytes - currentCountWidth);

    /* Step 1d: Calculate first attempt at TotalBytesWidth */
    int_fast8_t newTotalBytesWidth =
        flexVarintSplitFullNoZeroLenEmbedded_(newTotalBytes);

    /* Step 1e: Detect if byte storage width is changing.  If so, update
     *          byte lengths and re-run width calculation since changing the
     *          storage length can also change the number of bytes required
     *          to store the storage length.
     *          Repeat until no more TotalBytesWidth changes are detected. */
    int_fast8_t bytesStorageDiff = newTotalBytesWidth - currentTotalBytesWidth;

    /* yes, this *must* be a while loop because updating the size of
     * our storage width can change the size of the storage width itself,
     * which we can only discover by checking the width difference again. */
    int_fast8_t prevBytesWidth = newTotalBytesWidth;
    while (bytesStorageDiff != 0) {
        newTotalBytes += bytesStorageDiff;
        newTotalBytesWidth =
            flexVarintSplitFullNoZeroLenEmbedded_(newTotalBytes);
        bytesStorageDiff = (newTotalBytesWidth - prevBytesWidth);
        prevBytesWidth = newTotalBytesWidth;
    }

    /* Step 1f: Calculate the growth (or shrink) of all header metadata. */
    const int_fast8_t growBy = (newTotalBytesWidth + newCountWidth) -
                               (currentTotalBytesWidth + currentCountWidth);

    /* Step 2: If we need to grow (or shrink) varint bytes, get er done */
    if (growBy != 0) {
        /* OLD layout: [BYTES][COUNT]
         * NEW layout: [BYTES][COUNT][GROW]
         *      Fixup: [NEWBYTES][NEWCOUNT] */
        const uint_fast8_t currentDataStartOffset =
            (currentTotalBytesWidth + currentCountWidth);
        const uint_fast8_t newDataStartOffset = currentDataStartOffset + growBy;
        assert(newBytes >= currentDataStartOffset);

        const size_t moveBytes = newBytes - currentDataStartOffset;

        D("[(%d -> %d) [%zu -> %zu]; (%d -> %d) [%zu -> %zu)]; %d]: "
          "memmove(%d, %d, (%zu - %u) = %zu);\n",
          currentTotalBytesWidth, newTotalBytesWidth, flexTotalBytes_(f),
          newTotalBytes, currentCountWidth, newCountWidth, flexCount_(f),
          newCount, growBy, newDataStartOffset, currentDataStartOffset,
          newBytes, currentDataStartOffset, moveBytes);

        /* If we are growing, realloc *then* memmove data up. */
        if (growBy > 0) {
            /* @own: caller; free=flexFree */
            *_f = zrealloc(*_f, newTotalBytes);
            f = *_f;
            memmove(f + newDataStartOffset, f + currentDataStartOffset,
                    moveBytes);
        } else {
            /* else, shrinking: memmove data down *then* realloc to fit. */
            /* note: smart allocators will try to shrink allocations in-place
             * so this realloc ideally won't invoke a new memcpy. */
            memmove(f + newDataStartOffset, f + currentDataStartOffset,
                    moveBytes);
            if (!drain) {
                /* @own: caller; free=flexFree */
                *_f = zrealloc(*_f, newTotalBytes);
                f = *_f;
            }
        }
    } else {
        /* else, this is a regular resize with no varint grow/shrink */
        if (!drain) {
            /* @own: caller; free=flexFree */
            *_f = zrealloc(*_f, newBytes);
            f = *_f;
        }
    }

    /* Step 3: Set total bytes and total count */
    /* Set total bytes */
    int_fast8_t encodedLen;
    varintSplitFullNoZeroPut_(f, encodedLen, newTotalBytes);
    assert(encodedLen == newTotalBytesWidth);

    /* Set count */
    varintTaggedPut64FixedWidthQuick_(f + encodedLen, newCount, newCountWidth);

    D("...after update: Bytes: %zu (%d) [%zu]; Count: %zu (%d) [%zu]\n",
      flexTotalBytes_(f), newTotalBytesWidth, newBytes, flexCount_(f),
      newCountWidth, newCount);

    return growBy;
}

/* ====================================================================
 * flex entry struct readers
 * ==================================================================== */
/* Populate a struct with all information about an entry. */
DK_INLINE_ALWAYS void flexEntryDataPopulate(const flexEntry *restrict fe,
                                            flexEntryData *restrict fed) {
    FLEX_DECODE_LENGTH_FORWARD(fe, fed->encodingSize, fed->len);
    fed->encoding = fe[0];
    fed->fe = fe;
}

/* A negative encoding stores value v as the magnitude -v - 1.  In
 * FLEX_NEG_64B a magnitude up to INT64_MAX is a 64-bit value; one with
 * bit 63 set is a value in [-2^64, -2^63 - 1], which only a 128-bit type
 * holds.  The encoder stores those values as FLEX_NEG_96B, but the format
 * accepts both, so wideness is a property of the entry, not of its
 * encoding byte alone.  The payload is little-endian, so bit 63 is the
 * top bit of its last byte. */
DK_INLINE_ALWAYS bool flexNeg64IsWide_(const flexEntry *fe) {
    return fe[0] == FLEX_NEG_64B && (fe[1 + sizeof(uint64_t) - 1] & 0x80);
}

/* FLEX_NEG_128B magnitudes up to INT128_MAX cover the values
 * [INT128_MIN, -1].  A magnitude with bit 127 set stands for a value
 * below INT128_MIN, which no databox holds, so the format does not
 * contain it.  FLEX_NEG_96B needs no such check: its largest magnitude,
 * 2^96 - 1, is the value -2^96. */
DK_INLINE_ALWAYS bool flexNeg128OutOfRange_(const flexEntry *fe) {
    return fe[0] == FLEX_NEG_128B && (fe[1 + sizeof(__uint128_t) - 1] & 0x80);
}

/* True when the entry at 'fe' decodes to DATABOX_SIGNED_128 or
 * DATABOX_UNSIGNED_128. */
DK_INLINE_ALWAYS bool flexEntryIsIntegerWide_(const flexEntry *fe) {
    return FLEX_IS_INTEGER_BIG(fe[0]) || flexNeg64IsWide_(fe);
}

/* True when the entry at 'fe' decodes to DATABOX_SIGNED_64 or
 * DATABOX_UNSIGNED_64. */
DK_INLINE_ALWAYS bool flexEntryIsIntegerNarrow_(const flexEntry *fe) {
    return FLEX_IS_INTEGER(fe[0]) && !flexNeg64IsWide_(fe);
}

/* Return total number of bytes used by entry at 'fe'. */
DK_INLINE_ALWAYS size_t flexRawEntryLength(flexEntry *const fe) {
    size_t len;
    varintWidth encodingSize;
    FLEX_DECODE_LENGTH_FORWARD(fe, encodingSize, len);
    return len + FLEX_ENCODING_SIZE_TOTAL_FORWARD(fe, encodingSize);
}

/* Return total number of bytes used by entry at 'fe'. */
DK_FN_UNUSED DK_INLINE_ALWAYS size_t
flexRawEntryLengthReverse(flexEntry *const fe) {
    size_t len;
    varintWidth encodingSize;
    FLEX_DECODE_LENGTH_REVERSE(fe, encodingSize, len);
    return len + FLEX_ENCODING_SIZE_TOTAL_REVERSE(fe, encodingSize);
}

/* ====================================================================
 * flex internal accounting
 * ==================================================================== */
/* Payload size for every encoding whose size depends on the encoding
 * byte alone: fixed-width numbers and references plus the immediates.
 * Every other value is absent: the container tags (sized by their inner
 * varint), FLEX_SAME, the reserved compressed container tags,
 * FLEX_REAL_32D/64D, and the MDSC references when FLEX_ENABLE_PTR_MDSC
 * is off.
 *
 * This one case list expands into two switches: the decode-size switch
 * (flexDataSizeForFixedWidthEncodingWithInnerEntry, which adds the
 * container cases and asserts on anything else) and
 * flexFixedWidthPayloadSize (used by flexRawEntryLengthSafe, which
 * rejects anything else).  Sharing the list keeps the decoder and the
 * validator in step; expanding it in place keeps the decode path a
 * single jump table.  'SIZE_IS(n)' must leave the switch. */
#if FLEX_ENABLE_PTR_MDSC
#define FLEX_FIXED_WIDTH_MDSC_CASES_(SIZE_IS)                                  \
    case FLEX_EXTERNAL_MDSC_48B:                                               \
        SIZE_IS(48 / 8);                                                       \
    case FLEX_EXTERNAL_MDSC_64B:                                               \
        SIZE_IS(64 / 8);
#else
#define FLEX_FIXED_WIDTH_MDSC_CASES_(SIZE_IS)
#endif

#define FLEX_FIXED_WIDTH_CASES_(encoding, SIZE_IS)                             \
    case FLEX_NEG_8B:                                                          \
    case FLEX_NEG_16B:                                                         \
    case FLEX_NEG_24B:                                                         \
    case FLEX_NEG_32B:                                                         \
    case FLEX_NEG_40B:                                                         \
    case FLEX_NEG_48B:                                                         \
    case FLEX_NEG_56B:                                                         \
    case FLEX_NEG_64B:                                                         \
    case FLEX_UINT_8B:                                                         \
    case FLEX_UINT_16B:                                                        \
    case FLEX_UINT_24B:                                                        \
    case FLEX_UINT_32B:                                                        \
    case FLEX_UINT_40B:                                                        \
    case FLEX_UINT_48B:                                                        \
    case FLEX_UINT_56B:                                                        \
    case FLEX_UINT_64B:                                                        \
        SIZE_IS((size_t)EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding));       \
    case FLEX_UINT_96B:                                                        \
    case FLEX_NEG_96B:                                                         \
        SIZE_IS(96 / 8);                                                       \
    case FLEX_UINT_128B:                                                       \
    case FLEX_NEG_128B:                                                        \
        SIZE_IS(128 / 8);                                                      \
    case FLEX_REAL_B16B:                                                       \
    case FLEX_REAL_16B:                                                        \
        SIZE_IS(16 / 8);                                                       \
    case FLEX_REAL_32B:                                                        \
        SIZE_IS(32 / 8);                                                       \
    case FLEX_REAL_64B:                                                        \
        SIZE_IS(64 / 8);                                                       \
        FLEX_FIXED_WIDTH_MDSC_CASES_(SIZE_IS)                                  \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_8:                                  \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_16:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_24:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_32:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_40:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_48:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_56:                                 \
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_64:                                 \
        SIZE_IS((size_t)EXTERNAL_VARINT_WIDTH_FROM_REFERENCE_(encoding));      \
    case FLEX_BYTES_EMPTY:                                                     \
    case FLEX_TRUE:                                                            \
    case FLEX_FALSE:                                                           \
    case FLEX_NULL:                                                            \
        /* immediate encoding, no value */                                     \
        SIZE_IS(0);

/* Returns false for every encoding the table above does not size. */
DK_INLINE_ALWAYS bool flexFixedWidthPayloadSize(const flexEncoding encoding,
                                                size_t *const outSize) {
#define FLEX_FIXED_WIDTH_STORE_(size)                                          \
    do {                                                                       \
        *outSize = (size);                                                     \
        return true;                                                           \
    } while (0)

    switch (encoding) {
        FLEX_FIXED_WIDTH_CASES_(encoding, FLEX_FIXED_WIDTH_STORE_)
    default:
        return false;
    }

#undef FLEX_FIXED_WIDTH_STORE_
}

/* Return bytes needed to store fixed type encoded by 'encoding' */
DK_INLINE_ALWAYS size_t flexDataSizeForFixedWidthEncodingWithInnerEntry(
    const flexEncoding encoding, const flexEntry *innerFe,
    varintWidth *encodingSize, const bool isForward) {
    assert(encoding >= FLEX_NEG_8B);

    if (encodingSize) {
        *encodingSize = 1; /* ONE side of the encoding */
    }

#define FLEX_FIXED_WIDTH_RETURN_(size) return (size)

    switch (encoding) {
        FLEX_FIXED_WIDTH_CASES_(encoding, FLEX_FIXED_WIDTH_RETURN_)
    case FLEX_CONTAINER_MAP:
    case FLEX_CONTAINER_LIST:
    case FLEX_CONTAINER_SET:
    case FLEX_CONTAINER_TUPLE: {
        size_t len;
        varintWidth lensize;
        if (isForward) {
            varintSplitFullNoZeroGet_(innerFe, lensize, len);
        } else {
            varintSplitFullNoZeroReversedGet_(innerFe, lensize, len);
        }

        /* [DATA WITH EMBEDDED LENGTH][REVERSE LENGTH] */
        return len + lensize;
    }
    default:
        /* Writers never emit an encoding outside the table, and the
         * validators reject one. */
        assert(NULL);
        __builtin_unreachable();
    }

#undef FLEX_FIXED_WIDTH_RETURN_
}

DK_STATIC DK_FN_CONST uint_fast8_t flexEncodingLength(flexEncoding encoding,
                                                      size_t rawlen) {
    if (FLEX_IS_STR(encoding)) {
        varintWidth len;
        varintSplitFullNoZeroLength_(len, rawlen);
        return len;
    }

    /* else, we are using a fixed encoding type, and all fixed
     * encodings have their 'encoding' represented by 1 byte. */
    return 1 + 0;
}

DK_STATIC uint_fast8_t flexWriteEncoding(flexEntry *const fe,
                                         const flexInsertContents *contents) {
    if (FLEX_IS_STR(contents->encoding)) {
        /* write varint encoding of string length */
        varintWidth width;
        varintSplitFullNoZeroPut_(fe, width, contents->len);
        return width;
    }

    /* write fixed encoding */
    fe[0] = contents->encoding;
    return 1;
}

DK_STATIC uint_fast8_t flexWriteEncodingReversedForward(
    flexEntry *const fe, const flexInsertContents *contents) {
    if (FLEX_IS_STR(contents->encoding)) {
        varintWidth width;
        varintSplitFullNoZeroReversedPutForward_(fe, width, contents->len);
        return width;
    }

    fe[0] = contents->encoding;
    return 1;
}

#define divCeil(a, b) (((a) + (b) - 1) / (b))
DK_STATIC DK_FN_CONST flexEncoding flexEncodingUnsigned(uint64_t value) {
    /* Determine the smallest encoding for 'value' */

    /* Our unsigned encodings are 2 entries apart in the type map,
     * so for each byte we remove from 'value', we increase our encoding
     * up one size (which is +2 because our sizes are 2 entries apart). */

    flexEncoding encoding = FLEX_UINT_8B;
#if 0
    while ((value >>= 8) != 0) {
        encoding += FLEX_INTEGER_ENCODING_STEP;
    }
#else
    /* 'if' is necessary because __builtin_clzll is not defined for 0 */
    if (value) {
        /* We want the leading number of bytes set to zero, so:
         *   - 64 bits - number of leading zero bits in 'value'
         *   - ceiling divided by 8 to get the full number of bytes used
         *   - subtract 1 because of the implied initial UINT_8B position
         *   - then multiply by how many encoding steps to take for the end byte
         *     storage quantity result */
        encoding += (DK_BYTESUSED(value) - 1) * FLEX_INTEGER_ENCODING_STEP;
    }
#endif

    D("SET UNSIGNED ENCODING TO: %d FOR %" PRIu64 "\n", encoding, value);
    return encoding;
}

DK_STATIC DK_FN_CONST flexEncoding flexEncodingUnsignedBig(__uint128_t value) {
    /* Determine the smallest encoding for 'value' */

    const uint64_t topUse = value >> 64;
    if (topUse) {
        size_t usedBytes =
            divCeil(((sizeof(topUse) * 8) - __builtin_clzll(topUse)), 8);

        if (usedBytes) {
            /* Account for the lower half of the int128_t used storage too */
            usedBytes += sizeof(uint64_t);

            /* Our two Big storage classes are 96 bits and 128 bits */
            if (usedBytes > (96 / 8)) {
                /* Used more than 12 bytes, so need to store as 16 bytes */
                return FLEX_UINT_128B;
            }

            /* else, we are storing more than 8 bytes, but <= 12 bytes */
            return FLEX_UINT_96B;
        }
    }

    /* No value in top half, so just return lower half encoding instead */
    assert((value >> 64) == 0);
    return flexEncodingUnsigned((uint64_t)value);
}

#define REFERENCE_MAX_8 UINT8_MAX
#define REFERENCE_MAX_16 (REFERENCE_MAX_8 + UINT16_MAX)
#define REFERENCE_MAX_24 (REFERENCE_MAX_16 + ((1 << 24) - 1))
#define REFERENCE_MAX_32 ((uint64_t)REFERENCE_MAX_24 + ((1ULL << 32) - 1))
#define REFERENCE_MAX_40 ((uint64_t)REFERENCE_MAX_32 + ((1ULL << 40) - 1))
#define REFERENCE_MAX_48 ((uint64_t)REFERENCE_MAX_40 + ((1ULL << 48) - 1))
#define REFERENCE_MAX_56 ((uint64_t)REFERENCE_MAX_48 + ((1ULL << 56) - 1))
#define REFERENCE_MAX_64 (UINT64_MAX)

DK_STATIC
DK_FN_CONST uint64_t flexEncodingReferenceUnsignedEncode(uint64_t value) {
    /* TODO: we could rearrange this to be binary search */
    if (value <= REFERENCE_MAX_8) {
        return value;
    }

    if (value <= REFERENCE_MAX_16) {
        return value - REFERENCE_MAX_8;
    }

    if (value <= REFERENCE_MAX_24) {
        return value - REFERENCE_MAX_16;
    }

    if (value <= REFERENCE_MAX_32) {
        return value - REFERENCE_MAX_24;
    }

    if (value <= REFERENCE_MAX_40) {
        return value - REFERENCE_MAX_32;
    }

    if (value <= REFERENCE_MAX_48) {
        return value - REFERENCE_MAX_40;
    }

    if (value <= REFERENCE_MAX_56) {
        return value - REFERENCE_MAX_48;
    }

    if (value <= REFERENCE_MAX_64) {
        /* subtract the previous tier's max (56), matching the decode
         * side which adds REFERENCE_MAX_56 for the 64-bit encoding */
        return value - REFERENCE_MAX_56;
    }

    assert(NULL);
    __builtin_unreachable();
}

DK_STATIC DK_FN_CONST uint64_t
flexEncodingReferenceUnsignedDecode(flexEncoding encoding, uint64_t value) {
    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_8) {
        return value;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_16) {
        return value + REFERENCE_MAX_8;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_24) {
        return value + REFERENCE_MAX_16;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_32) {
        return value + REFERENCE_MAX_24;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_40) {
        return value + REFERENCE_MAX_32;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_48) {
        return value + REFERENCE_MAX_40;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_56) {
        return value + REFERENCE_MAX_48;
    }

    if (encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_64) {
        return value + REFERENCE_MAX_56;
    }

    assert(NULL);
    __builtin_unreachable();
}

DK_STATIC DK_FN_CONST flexEncoding
flexEncodingReferenceUnsigned(uint64_t encodedValue) {
    if (encodedValue <= REFERENCE_MAX_8) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_8;
    }

    if (encodedValue <= REFERENCE_MAX_16) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_16;
    }

    if (encodedValue <= REFERENCE_MAX_24) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_24;
    }

    if (encodedValue <= REFERENCE_MAX_32) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_32;
    }

    if (encodedValue <= REFERENCE_MAX_40) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_40;
    }

    if (encodedValue <= REFERENCE_MAX_48) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_48;
    }

    if (encodedValue <= REFERENCE_MAX_56) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_56;
    }

    if (encodedValue <= REFERENCE_MAX_64) {
        return FLEX_CONTAINER_REFERENCE_EXTERNAL_64;
    }

    assert(NULL);
    __builtin_unreachable();
}

/* increase the negative number by one because we don't store
 * signed zero and can use '0' to store '-1', etc.  The writer stores the
 * negation of the result, so a negative value v is stored as the
 * magnitude -v - 1, and the decoder restores it as -magnitude - 1. */
#define SIGNED_PREPARE(v) ((v) + 1)

DK_STATIC DK_FN_CONST inline int64_t flexPrepareSigned(int64_t value) {
    /* We don't store "signed zero," so we can save
     * one integer position on all negative numbers.
     *
     * This also protects us from attempting to store INT64_MIN
     * in an unsigned quantity since this will convert it into
     * an opposite-sign-safe representation. */
    if (value < 0) {
        return SIGNED_PREPARE(value);
    }

    return value;
}

DK_STATIC DK_FN_CONST inline __int128_t flexPrepareSignedBig(__int128_t value) {
    /* We don't store "signed zero," so we can save
     * one integer position on all negative numbers.
     *
     * This also protects us from attempting to store INT64_MIN
     * in an unsigned quantity since this will convert it into
     * an opposite-sign-safe representation.
     *
     * Returns __int128_t (NOT int64_t): both callers store the result into a
     * full 128-bit slot (the SIGNED_128 insert + the sorted-insert scratch
     * buffer), so an int64_t return silently truncated every 128-bit value to
     * its low 64 bits before it was ever encoded. */
    if (value < 0) {
        return SIGNED_PREPARE(value);
    }

    return value;
}

DK_STATIC DK_FN_CONST flexEncoding flexEncodingSigned(int64_t value) {
    /* Determine the smallest encoding for 'value' */

    if (value < 0) {
        /* Convert signed to unsigned in proper range */
        /* Minus one because we don't need to store a signed zero,
         * so we adjust all values by one. */
        const uint64_t converted = DK_INT64_TO_UINT64(value) - 1;

        /* To save us from having to compare 16 ranges, just:
         *   - turn negative number positive (unsigned)
         *   - compare in unsigned range
         *   - convert unsigned type to negative type */
        /* Our negative type IDs are one minus their unsigned counterparts. */
        return flexEncodingUnsigned(converted) - 1;
    }

    return flexEncodingUnsigned(value);
}

DK_STATIC DK_FN_CONST flexEncoding flexEncodingSignedBig(__int128_t value) {
    /* Determine the smallest encoding for 'value' */

    if (value < 0) {
        /* Convert signed to unsigned in proper range */
        /* Minus one because we don't need to store a signed zero,
         * so we adjust all values by one. */
        const __uint128_t converted = DK_INT128_TO_UINT128(value) - 1;

        /* A 64-bit negative encoding decodes into an int64_t, so it holds
         * stored magnitudes up to INT64_MAX (values down to INT64_MIN).
         * A value below INT64_MIN is wide and needs a wide encoding, even
         * when its magnitude would fit 64 unsigned bits. */
        if (converted > INT64_MAX && (converted >> 64) == 0) {
            return FLEX_NEG_96B;
        }

        /* To save us from having to compare 16 ranges, just:
         *   - turn negative number positive (unsigned)
         *   - compare in unsigned range
         *   - convert unsigned type to negative type */
        /* Our negative type IDs are one minus their unsigned counterparts. */
        return flexEncodingUnsignedBig(converted) - 1;
    }

    return flexEncodingUnsignedBig(value);
}

#define realFits16(value) ((value) == float16Decode(float16Encode(value)))
#define realFitsB16(value) ((value) == bfloat16Decode(bfloat16Encode(value)))

DK_STATIC flexEncoding flexEncodingFloat(const float value) {
    if (realFits16(value)) {
        return FLEX_REAL_16B;
    }

    if (realFitsB16(value)) {
        return FLEX_REAL_B16B;
    }

    return FLEX_REAL_32B;
}

DK_STATIC flexEncoding flexEncodingDouble(double value) {
    /* Attempt to encode double to float with no loss of precision */
    if ((double)(float)value == value) {
        /* Sucess! Now try to encode to REAL_16 or REAL_B16 too. */
        return flexEncodingFloat(value);
    }

    return FLEX_REAL_64B;
}

/* ====================================================================
 * flex internal physical writing
 * ==================================================================== */
/* Store float 'value' at 'fe' */
DK_STATIC void flexSaveFloat16(flexData *const fe, const float value) {
    const uint16_t writer = float16Encode(value);
    memcpy(fe, &writer, sizeof(writer));
    conformToLittleEndian16(*fe);
    D("SAVING HALF FLOAT AS 2 bytes\n");
}

DK_STATIC void flexSaveFloatB16(flexData *const fe, const float value) {
    const uint16_t writer = bfloat16Encode(value);
    memcpy(fe, &writer, sizeof(writer));
    conformToLittleEndian16(*fe);
    D("SAVING BHALF FLOAT AS 2 bytes\n");
}

DK_STATIC void flexSaveFloat(flexData *const fe, const float value) {
    memcpy(fe, &value, sizeof(value));
    conformToLittleEndian32(*fe);
    D("SAVING FLOAT AS 4 bytes\n");
}

/* Store double 'value' at 'fe' */
DK_STATIC void flexSaveDouble(flexData *const fe, const double value) {
    memcpy(fe, &value, sizeof(value));
    conformToLittleEndian64(*fe);
    D("SAVING DOUBLE AS 8 bytes\n");
}

DK_STATIC size_t flexWritePayload(flexEntry *const fe,
                                  const flexInsertContents *contents) {
    const flexEncoding encoding = contents->encoding;
    const void *data = contents->data;
    const size_t len = contents->len;

    if (FLEX_IS_STR(encoding)) {
        /* If 'fe' == 'data', then there's nothing new to write!
         * else, copy 'len' 'data' to 'fe' */
        if (fe != data) {
            /* memove (instead of memcpy()) because 'fe' could
             * be inside the range of 'data' + 'len' */
            memmove(fe, data, len);
        }

        return len;
    }

    if (unlikely(contents->isVoidEntry)) {
        /* If user requested just an abstract space allocation without
         * needing any contents written, then just report we made 'len'
         * bytes available for the user (they'll get the entry and write
         * contents later). */
        return len;
    }

    varintWidth width = EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding);
    switch (encoding) {
    case FLEX_NEG_8B:
    case FLEX_NEG_16B:
    case FLEX_NEG_24B:
    case FLEX_NEG_32B:
    case FLEX_NEG_40B:
    case FLEX_NEG_48B:
    case FLEX_NEG_56B:
    case FLEX_NEG_64B:
        /* varints are unsigned 64 bit integers.  If we cast a negative
         * number to unsigned, it grows really big due to the sign bit
         * being in the topmost set bit position.
         * For storage, we _properly_ convert the negaive integer to a positive
         * integer for deconstructing the unused bytes.
         * We _already_ did the conversion to "acceptable range of int64_t for
         * reversing sign to a uint64_t" before getting here by using
         * flexPrepareSigned elsewhere. */
        varintExternalPutFixedWidthQuick_(fe, -(*(int64_t *)data), width);
        return width;
    case FLEX_NEG_96B:
    case FLEX_NEG_128B:
        varintExternalPutFixedWidthBig(fe, -*(__uint128_t *)data, width);
        return width;
    case FLEX_UINT_8B:
    case FLEX_UINT_16B:
    case FLEX_UINT_24B:
    case FLEX_UINT_32B:
    case FLEX_UINT_40B:
    case FLEX_UINT_48B:
    case FLEX_UINT_56B:
    case FLEX_UINT_64B:
        /* We always pass integers as 8 byte quantities, so we can
         * safely read the entire 8 bytes here into varintExternalPut() */
        varintExternalPutFixedWidthQuick_(fe, *(uint64_t *)data, width);
        return width;
    case FLEX_UINT_96B:
    case FLEX_UINT_128B:
        varintExternalPutFixedWidthBig(fe, *(__uint128_t *)data, width);
        return width;
    case FLEX_REAL_B16B:
        flexSaveFloatB16(fe, *(float *)data);
        return 16 / 8;
    case FLEX_REAL_16B:
        flexSaveFloat16(fe, *(float *)data);
        return 16 / 8;
    case FLEX_REAL_32B:
        flexSaveFloat(fe, *(float *)data);
        return 32 / 8;
    case FLEX_REAL_64B:
        flexSaveDouble(fe, *(double *)data);
        return 64 / 8;
#if FLEX_ENABLE_PTR_MDSC
    case FLEX_EXTERNAL_MDSC_48B:
        varintExternalPutFixedWidth(fe, *(uint64_t *)data, 6);
        return 48 / 8;
    case FLEX_EXTERNAL_MDSC_64B:
        varintExternalPutFixedWidth(fe, *(uint64_t *)data, 8);
        return 64 / 8;
#endif
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_8:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_16:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_24:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_32:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_40:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_48:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_56:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_64:
        width = EXTERNAL_VARINT_WIDTH_FROM_REFERENCE_(encoding);
        varintExternalPutFixedWidthQuick_(fe, *(uint64_t *)data, width);
        return width;
    case FLEX_CONTAINER_MAP:
    case FLEX_CONTAINER_LIST:
    case FLEX_CONTAINER_SET:
    case FLEX_CONTAINER_TUPLE:
        /* write flex directly, it already starts with
         * a valid string length encoding */
        memmove(fe, data, len);

        /* write reverse length after the nested flex, where reverse
         * traversal reads it */
        varintWidth refWidth;
        varintSplitFullNoZeroReversedPutForward_(fe + len, refWidth, len);
        return len + refWidth;
    case FLEX_BYTES_EMPTY:
    case FLEX_TRUE:
    case FLEX_FALSE:
    case FLEX_NULL:
        /* immediate encoding, no value */
        return 0;
    default:
        assert(NULL); /* unsupported encoding */
        __builtin_unreachable();
    }
}

/* ====================================================================
 * flex internal physical reading
 * ==================================================================== */
/* Where the 96/128-bit arms of flexLoadFixedLength_ put a wide payload.
 * Narrow arms never look at it. */
typedef enum flexWideStore_ {
    /* Into the caller's 'slot', which the box then borrows (big = 1,
     * allocated = 0).  A NULL slot reports the value deferred: type only,
     * data.ptr = NULL, big = 0, allocated = 0. */
    FLEX_WIDE_STORE_SLOT_ = 0,
    /* Through the box's own pointer when big = 1, and deferred otherwise;
     * the flag bits are left as they were (flexGetByType). */
    FLEX_WIDE_STORE_IN_BAND_,
    /* As in band when big = 1, and otherwise into a new 16-byte heap block
     * the box owns, with allocated = 1 and big = 1 (flexGetByTypeCopy). */
    FLEX_WIDE_STORE_OWNED_,
} flexWideStore_;

/* Set up 'r' for a 96/128-bit value and return the storage to write it
 * through, or NULL to leave it deferred.  Only the wide arms call this,
 * so it stays out of line and off the narrow path. */
static __attribute__((noinline, cold)) void *
flexWideTarget_(databox *r, databoxWide *slot, const flexWideStore_ store) {
    switch (store) {
    case FLEX_WIDE_STORE_SLOT_:
        r->data.ptr = slot;
        r->big = slot != NULL;
        r->allocated = false;
        return slot;
    case FLEX_WIDE_STORE_IN_BAND_:
        if (!r->big) {
            /* A plain databox has no 128-bit store: report the type only,
             * and never write through whatever its pointer holds. */
            r->data.ptr = NULL;
            return NULL;
        }

        assert(r->data.ptr);
        return r->data.ptr;
    case FLEX_WIDE_STORE_OWNED_:
        if (!r->big) {
            /* @own: caller; free=databoxFreeData */
            __uint128_t *payload = zmalloc(sizeof(*payload));
            assert(((uintptr_t)payload % _Alignof(__uint128_t)) == 0);
            r->data.u128 = payload;
            r->big = true;
            r->allocated = true;
        }

        assert(r->data.ptr);
        return r->data.ptr;
    }

    __builtin_unreachable();
}

/* Read value pointed to by 'entry', store result in databox 'r'.  Narrow
 * arms write only type, data and (for variable-length types) len; a
 * 96/128-bit value finds its storage through flexWideTarget_. */
DK_STATIC void flexLoadFixedLength_(const flexEntryData *entry, databox *r,
                                    databoxWide *const slot,
                                    const flexWideStore_ store) {
    flexData *d = FLEX_ENTRY_DATA(entry);
    const flexEncoding encoding = entry->encoding;

#if 0
    /* Force clear 8 bytes holding {i,u}{8,16,32,64},f32,d64 by setting
     * the largest union value to zero. */
    r->data.u = 0;
#endif

    /* get data for types */
    switch (encoding) {
    case FLEX_UINT_8B:
    case FLEX_UINT_16B:
    case FLEX_UINT_24B:
    case FLEX_UINT_32B:
    case FLEX_UINT_40B:
    case FLEX_UINT_48B:
    case FLEX_UINT_56B:
    case FLEX_UINT_64B:
        varintExternalGetQuick_(
            d, EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding), r->data.u64);
        r->type = DATABOX_UNSIGNED_64;
        break;
    case FLEX_UINT_96B:
    case FLEX_UINT_128B: {
        r->type = DATABOX_UNSIGNED_128;
        __uint128_t *const out = flexWideTarget_(r, slot, store);
        if (out) {
            *out = varintBigExternalGet(d, encoding == FLEX_UINT_96B ? 96 / 8
                                                                     : 128 / 8);
        }
        break;
    }
    case FLEX_NEG_8B:
    case FLEX_NEG_16B:
    case FLEX_NEG_24B:
    case FLEX_NEG_32B:
    case FLEX_NEG_40B:
    case FLEX_NEG_48B:
    case FLEX_NEG_56B:
    case FLEX_NEG_64B: {
        uint64_t magnitude;
        varintExternalGetQuick_(
            d, EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding), magnitude);
        if (likely(magnitude <= INT64_MAX)) {
            r->type = DATABOX_SIGNED_64;
            r->data.i64 = -(int64_t)magnitude - 1;
            break;
        }

        /* A FLEX_NEG_64B value below INT64_MIN (see flexNeg64IsWide_). */
        r->type = DATABOX_SIGNED_128;
        __int128_t *const out = flexWideTarget_(r, slot, store);
        if (out) {
            *out = -(__int128_t)magnitude - 1;
        }
        break;
    }
    case FLEX_NEG_96B:
    case FLEX_NEG_128B: {
        r->type = DATABOX_SIGNED_128;
        __int128_t *const out = flexWideTarget_(r, slot, store);
        if (out) {
            const __uint128_t magnitude = varintBigExternalGet(
                d, encoding == FLEX_NEG_96B ? 96 / 8 : 128 / 8);
            /* -magnitude - 1 is ~magnitude.  Computing it unsigned keeps
             * the decode defined for a FLEX_NEG_128B magnitude above
             * INT128_MAX, which has no 128-bit value (it wraps); the
             * writer never stores one and flexValidateBufferDeep rejects
             * one (see flexNeg128OutOfRange_). */
            *out = (__int128_t)~magnitude;
        }
        break;
    }
    case FLEX_REAL_B16B:
        /* Restore 2 byte real to 4 byte real */
        memcpy(&r->data.u16, d, 2);
        r->data.f32 = bfloat16Decode(r->data.u16);
        r->type = DATABOX_FLOAT_32;
        break;
    case FLEX_REAL_16B:
        /* Restore 2 byte real to 4 byte real */
        memcpy(&r->data.u16, d, 2);
        r->data.f32 = float16Decode(r->data.u16);
        r->type = DATABOX_FLOAT_32;
        break;
    case FLEX_REAL_32B:
        memcpy(&r->data.f32, d, sizeof(r->data.f32));
        conformToLittleEndian32(r->data.f32);
        r->type = DATABOX_FLOAT_32;
        break;
    case FLEX_REAL_64B:
        memcpy(&r->data.d64, d, sizeof(r->data.d64));
        conformToLittleEndian64(r->data.d64);
        r->type = DATABOX_DOUBLE_64;
        break;
#if FLEX_ENABLE_PTR_MDSC
    case FLEX_EXTERNAL_MDSC_48B:
        r->data.uptr = varintExternalGet(d, 6);
        r->type = DATABOX_PTR_MDSC;
        r->len = mdsclen(r->data.ptr);
        break;
    case FLEX_EXTERNAL_MDSC_64B:
        r->data.uptr = varintExternalGet(d, 8);
        r->type = DATABOX_PTR_MDSC;
        r->len = mdsclen(r->data.ptr);
        break;
#endif
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_8:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_16:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_24:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_32:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_40:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_48:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_56:
    case FLEX_CONTAINER_REFERENCE_EXTERNAL_64:
        varintExternalGetQuick_(
            d, EXTERNAL_VARINT_WIDTH_FROM_REFERENCE_(encoding), r->data.u64);
        r->data.u64 =
            flexEncodingReferenceUnsignedDecode(encoding, r->data.u64);
        r->type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
        break;
    case FLEX_CONTAINER_MAP:
    case FLEX_CONTAINER_LIST:
    case FLEX_CONTAINER_SET:
    case FLEX_CONTAINER_TUPLE:
        r->type =
            DATABOX_CONTAINER_FLEX_MAP + FLEX_CONTAINER_OFFSET(entry->encoding);
        r->data.bytes.start = d;
        r->len = entry->len;
        break;
    case FLEX_BYTES_EMPTY:
        r->type = DATABOX_BYTES;
        r->len = 0;
        break;
    case FLEX_TRUE:
        r->type = DATABOX_TRUE;
        r->data.u = 1;
        break;
    case FLEX_FALSE:
        r->type = DATABOX_FALSE;
        r->data.u = 0;
        break;
    case FLEX_NULL:
        r->type = DATABOX_NULL;
        break;
    default:
        assert(NULL && "Invalid type detected for this use case!");
    }
}

/* The 64-bit loaders take narrow entries only: their box has no wide
 * storage. */
DK_STATIC int64_t flexLoadSigned(const flexEntryData *entry) {
    assert(!flexEntryIsIntegerWide_(entry->fe));
    databox box;
    flexLoadFixedLength_(entry, &box, NULL, FLEX_WIDE_STORE_SLOT_);
    return box.data.i64;
}

/* The wide loaders take wide entries only: the value lands in 'wide'. */
DK_STATIC __int128_t flexLoadSignedBig(const flexEntryData *entry) {
    databox box;
    databoxWide wide;
    flexLoadFixedLength_(entry, &box, &wide, FLEX_WIDE_STORE_SLOT_);
    assert(box.data.ptr == &wide);
    return *box.data.i128;
}

DK_STATIC uint64_t flexLoadUnsigned(const flexEntryData *entry) {
    assert(!flexEntryIsIntegerWide_(entry->fe));
    databox box;
    flexLoadFixedLength_(entry, &box, NULL, FLEX_WIDE_STORE_SLOT_);
    return box.data.u64;
}

DK_STATIC __uint128_t flexLoadUnsignedBig(const flexEntryData *entry) {
    databox box;
    databoxWide wide;
    flexLoadFixedLength_(entry, &box, &wide, FLEX_WIDE_STORE_SLOT_);
    assert(box.data.ptr == &wide);
    return *box.data.u128;
}

DK_STATIC float flexLoadFloat(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box, NULL, FLEX_WIDE_STORE_SLOT_);
    return box.data.f32;
}

DK_STATIC double flexLoadDouble(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box, NULL, FLEX_WIDE_STORE_SLOT_);
    return box.data.d64;
}

/* Create a new empty flex. */
flex *flexNew(void) {
    /* @own: caller; free=flexFree */
    flex *f = zcalloc(1, FLEX_EMPTY_HEADER_SIZE);
    (void)flexSetTotalBytesCount(&f, FLEX_EMPTY_HEADER_SIZE, 0);

#ifdef FLEX_DEBUG_EXTENSIVE
    assert(flexTotalBytes_(f) == FLEX_EMPTY_HEADER_SIZE);
#endif

    return f;
}

void flexReset(flex **const ff) {
    memset(*ff, 0, flexTotalBytes_(*ff));
    (void)flexSetTotalBytesCount(ff, FLEX_EMPTY_HEADER_SIZE, 0);
}

/* Free flex */
void flexFree(flex *f) {
    zfree(f);
}

/* ====================================================================
 * flex internal physical resizing
 * ==================================================================== */
/* Resize the flex.
 * If 'drain' is true, we don't realloc when shrinking since we are
 * emptying out the list one element at a time.  We don't want
 * N realloc operations when we're just going to free the list anyway. */
DK_INLINE_ALWAYS int_fast8_t flexResize_(flex **const _f,
                                         const size_t newLength,
                                         const int64_t countAdjustBy,
                                         const bool drain) {
    return flexSetTotalBytesCount_(_f, newLength,
                                   (flexCount_(*_f) + countAdjustBy), drain);
}

DK_INLINE_ALWAYS int_fast8_t flexResize(flex **const _f, const size_t newLength,
                                        const int64_t countAdjustBy) {
    return flexResize_(_f, newLength, countAdjustBy, false);
}

DK_INLINE_ALWAYS void flexBulkAppend(flex **const ff, const void *const data,
                                     const size_t len, const size_t addCount) {
    const size_t fLen = flexTotalBytes_(*ff);
    /* @own: caller; free=flexFree */
    *ff = zrealloc(*ff, fLen + len);

    memcpy(*ff + fLen, data, len);
    flexResize(ff, fLen + len, addCount);
}

void flexBulkAppendFlex(flex **const ff, const flex *const zzb) {
    const uint32_t zzbHeader = FLEX_HEADER_SIZE(zzb);
    flexBulkAppend(ff, zzb + zzbHeader, flexTotalBytes_(zzb) - zzbHeader,
                   flexCount(zzb));
}

/* Concatenate the CONTENTS of `count` flexes into one fresh flex.
 *
 * SIZE IS THE SUM OF PAYLOADS PLUS EXACTLY ONE HEADER.
 *
 * `flexTotalBytes_` INCLUDES an input's own header, so summing whole
 * totalBytes counted every input's header into the destination's
 * declared length while only ONE header is ever written.  The merged
 * flex then declared (sum of input headers - its own header) bytes it
 * never wrote: FLEX_ENTRY_END pointed past the last real entry into
 * uninitialised heap, flexBytes() over-reported, and any consumer that
 * walks to the end rather than counting entries read garbage.  Two
 * inputs was enough to trip it (>= 2 bytes of slack), and every extra
 * input added more.  Sum payloads instead, so the result is exactly
 * what the same entries appended one at a time would have produced.
 *
 * A zero-payload merge (count == 0, or every input empty) returns the
 * empty flex directly: resizing to a length below the header size is
 * not representable. */
flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count) {
    size_t payloadBytes = 0;
    size_t totalCount = 0;
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        payloadBytes += flexTotalBytes_(fs[i]) - FLEX_HEADER_SIZE(fs[i]);
        totalCount += flexCount_(fs[i]);
    }

    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f || payloadBytes == 0) {
        return f;
    }

    /* flexResize takes the FULL new length including the header it is
     * about to (re)size; the empty flex's header is the current one, so
     * the widening of both varints is computed from there. */
    flexResize(&f, FLEX_EMPTY_HEADER_SIZE + payloadBytes, totalCount);

    offset = FLEX_HEADER_SIZE(f);
    for (size_t i = 0; i < count; i++) {
        const size_t totalSizeCurrent = flexTotalBytes_(fs[i]);
        const size_t headerSizeCurrent = FLEX_HEADER_SIZE(fs[i]);
        const size_t copyBytes = totalSizeCurrent - headerSizeCurrent;
        memcpy(f + offset, fs[i] + headerSizeCurrent, copyBytes);
        offset += copyBytes;
    }

    return f;
}

/* Members per chunk for the bulk byte-member append below.
 *
 * Inside one chunk the appends are still exact-resize, so a chunk costs
 * O(membersPerChunk * chunkBytes); across count/membersPerChunk chunks
 * the total is O(membersPerChunk * totalBytes) — LINEAR in the payload
 * with a small constant, against O(totalBytes^2) for a single growing
 * flex.  Small keeps the constant low; above one keeps tiny members from
 * paying an allocation each. */
#define FLEX_BULK_PUSH_MEMBERS_PER_CHUNK 16

/* Chunk pointers held on the stack before spilling to one heap block. */
#define FLEX_BULK_PUSH_CHUNK_STACK_SLOTS 32

void flexBulkPushBytesMembers(flex **const ff,
                              const flexBytesMember *const members,
                              const size_t count) {
    if (!ff || !*ff || !members || count == 0) {
        return;
    }

    /* Small batches: the chunk+merge machinery costs more than the
     * quadratic term it removes, and the direct loop IS the definition
     * this function reproduces. */
    if (count <= FLEX_BULK_PUSH_MEMBERS_PER_CHUNK) {
        for (size_t i = 0; i < count; i++) {
            flexPushBytes(ff, members[i].data, members[i].len,
                          FLEX_ENDPOINT_TAIL);
        }
        return;
    }

    const size_t chunkCount = (count + FLEX_BULK_PUSH_MEMBERS_PER_CHUNK - 1) /
                              FLEX_BULK_PUSH_MEMBERS_PER_CHUNK;
    /* +1 slot: the caller's existing flex leads the merge, so whatever
     * it already holds keeps its position ahead of the new members. */
    const size_t mergeCap = chunkCount + 1;

    flex *chunkStack[FLEX_BULK_PUSH_CHUNK_STACK_SLOTS];
    flex **merge = chunkStack;
    cleanerName(zfree, FlexPtrs) flex **mergeHeap = NULL;
    if (mergeCap > (sizeof(chunkStack) / sizeof(chunkStack[0]))) {
        mergeHeap = zcalloc(mergeCap, sizeof(*merge));
        if (!mergeHeap) {
            for (size_t i = 0; i < count; i++) {
                flexPushBytes(ff, members[i].data, members[i].len,
                              FLEX_ENDPOINT_TAIL);
            }
            return;
        }

        merge = mergeHeap;
    }

    size_t mergeCount = 0;
    merge[mergeCount++] = *ff;

    bool chunkFail = false;
    for (size_t i = 0; i < count; i += FLEX_BULK_PUSH_MEMBERS_PER_CHUNK) {
        const size_t thisChunk = (count - i) < FLEX_BULK_PUSH_MEMBERS_PER_CHUNK
                                     ? (count - i)
                                     : FLEX_BULK_PUSH_MEMBERS_PER_CHUNK;
        /* @own: container; free=flexFree */
        flex *chunk = flexNew();
        if (!chunk) {
            chunkFail = true;
            break;
        }

        for (size_t j = 0; j < thisChunk; j++) {
            flexPushBytes(&chunk, members[i + j].data, members[i + j].len,
                          FLEX_ENDPOINT_TAIL);
        }

        merge[mergeCount++] = chunk;
    }

    flex *merged =
        chunkFail
            ? NULL
            : flexBulkMergeFlex((const flex *const *const)merge, mergeCount);

    /* Free the chunks only — slot 0 is the caller's flex, which stays
     * theirs until the merge has actually succeeded. */
    for (size_t i = 1; i < mergeCount; i++) {
        flexFree(merge[i]);
    }

    if (!merged) {
        /* Assembly failed with `*ff` untouched, so the slow path still
         * produces the correct result rather than a partial one. */
        for (size_t i = 0; i < count; i++) {
            flexPushBytes(ff, members[i].data, members[i].len,
                          FLEX_ENDPOINT_TAIL);
        }
        return;
    }

    flexFree(*ff);
    *ff = merged;
}

/* ====================================================================
 * flex deletion
 * ==================================================================== */
typedef struct flexHeaderInfo {
    /* 'insertedBytes' is the number of bytes added or removed
     * during an insert or delete operation.
     * It's possible an insert option will delete bytes if we're replacing
     * a large value with a smaller value (so we have to remove the
     * extra bytes) */
    int64_t insertedBytes;

    /* Header size can (shrink on delete) or (grow or shrink on insert). */
    int_fast8_t headerDiff;
} flexHeaderInfo;

#define flexDelete__(z, fe, n, d) flexDelete___(z, fe, n, d, NULL, NULL)
#define flexDelete__GetDeleted(z, fe, n, d, dd)                                \
    flexDelete___(z, fe, n, d, NULL, dd)

DK_STATIC void flexDelete___(flex **const ff, flexEntry *fe, int_fast32_t count,
                             const bool drain, flexHeaderInfo *headerInfo,
                             flex **const placeDeletedContentsHere) {
    assert(fe);

    const flexEntry *end = GET_END(*ff);
    flexEntry *initialFE = fe;

    int_fast32_t deletedCount = 0;
    while (count--) {
        fe += flexRawEntryLength(fe);
        deletedCount++;
        if (fe == end) {
            break;
        }
    }

    const ssize_t totalBytesRemoved = fe - initialFE;
    const size_t totalBytes = flexTotalBytes_(*ff);
    assert((size_t)totalBytesRemoved < totalBytes);

    if (placeDeletedContentsHere) {
        flexBulkAppend(placeDeletedContentsHere, initialFE, totalBytesRemoved,
                       deletedCount);
    }

    /* Move bytes after 'fe' down to cover the deleted entries. */
    const size_t offset = fe - *ff;
    memmove(initialFE, fe, totalBytes - offset);

    /* Now resize the flex and update element counts */
    const int_fast8_t inserted =
        flexResize_(ff, totalBytes - totalBytesRemoved, -deletedCount, drain);

    if (headerInfo) {
        headerInfo->insertedBytes = -totalBytesRemoved;
        headerInfo->headerDiff = inserted;
    }
}

/* ====================================================================
 * flex insert
 * ==================================================================== */
/* Obtain the *complete* write size for 'contents'.
 * Complete means: forward encoding, data size, reverse encoding. */
DK_INLINE_ALWAYS size_t
abstractInsertSizeFromInsertContents(flexInsertContents *contents) {
    size_t insertSize = 0;

#if 0
    if (FLEX_IS_IMMEDIATE(contents->encoding)) {
        contents->encodingLen = 1;
        return 1;
    }
#endif

    /* Here, 'contents->data' is only used to lookup the inner size of
     * an embedded FLEX if our type is DATABOX_CONTAINER_FLEX_* */
    insertSize += FLEX_IS_STR(contents->encoding)
                      ? contents->len
                      : flexDataSizeForFixedWidthEncodingWithInnerEntry(
                            contents->encoding, contents->data, NULL, true);

    const uint_fast8_t encodingSize =
        flexEncodingLength(contents->encoding, contents->len);

    contents->encodingLen = encodingSize;

    insertSize += FLEX_ENCODING_SIZE_TOTAL_FROM_ENCODING(contents->encoding,
                                                         encodingSize);

    return insertSize;
}

DK_INLINE_ALWAYS void
abstractWriteFullEntry(flexEntry *restrict *restrict fe,
                       const flexInsertContents *restrict contents) {
    /* Now, the *actual* insert operation: write data to the flex. */
    /* Write the full entry in three parts: [ENCODING][DATA][GNIDOCNE] */

    /* Part one: ENCODING */
    *fe += flexWriteEncoding(*fe, contents);

    /* If encoding is immediate (true/false/null), we are *only* one
     * encoding byte, so skip other writes. */
    if (!FLEX_IS_IMMEDIATE(contents->encoding)) {
        /* Part two: USER DATA */
        *fe += flexWritePayload(*fe, contents);

        /* Part three: ENCODING again, but reversed this time. */
        *fe += flexWriteEncodingReversedForward(*fe, contents);
    }
}

#define flexInsert_(ff, fe, encoding, data, len)                               \
    flexInsert____(ff, fe, encoding, data, len, false, NULL)

#define flexInsertReplace_(ff, fe, encoding, data, len)                        \
    flexInsert____(ff, fe, encoding, (void *)(data), len, true, NULL)

/* ====================================================================
 * INSERT COMMONALITY
 * ==================================================================== */
/* Note:
 *   These *UGLY* macros only exist for code commonality.
 *   Insert() and Replace() require about 80% of the same work, but the other
 *   20% is individual to each insert type.  We get a big speedup by *not*
 *   having one 200 line InsertWithOptionalReplace() function (with branches
 *   every 8 lines depending on if insert or if insert-with-replace) and instead
 *   just creating single purpose insert functions.
 *
 *   It also improves readability having the two Insert() functions split up by
 *   purpose even though they share most of the same behavioral code.
 *
 *   These macros are only used in very specific places so we don't care about
 *   them being generally reusable.
 *
 *   In fact, we depend on all these exact symbols springing into existence
 *   based on the below very specific, non-generally-reusable, macros. */
#define commonFlexInsertCalculatePreconditions_(contentsStartOffset)           \
    /* If 'data' is *inside* the target flex (e.g. duplicating a current       \
     * element in the same flex), we must do additional                        \
     * accounting to preserve the position of 'data' after our reallocations   \
     * and memmoves below. */                                                  \
    ssize_t flexSize = flexTotalBytes_(f);                                     \
    bool copyFromSelf = false;                                                 \
    /* We only use contents[0] for replaceOffset calculations. */              \
    const uint8_t *data = contents[0]->data;                                   \
    int64_t offsetData = data - f;                                             \
    const bool dataIsAfterFe = data > fe;                                      \
    bool feIsEntryHoldingData = false;                                         \
    if ((data > f) && (data < (f + flexSize))) {                               \
        copyFromSelf = true;                                                   \
        /* An append position (fe at the end) holds no entry to read. */       \
        if (fe < f + flexSize) {                                               \
            feIsEntryHoldingData =                                             \
                ((fe + FLEX_LENGTH_OF_ENCODING(fe)) == data);                  \
        }                                                                      \
    }                                                                          \
                                                                               \
    /* Per-i self-aliasing tracking for contents[i] in                         \
     * [contentsStartOffset .. contentsCount-1].  The legacy macro only        \
     * rebased contents[0]->data after realloc; the replace path               \
     * (contentsStartOffset==1) writes contents[1..N-1] which can also         \
     * alias inside this same flex (zero-copy databox views built from         \
     * a prior in-place read on the same key, e.g. SET-after-GET round         \
     * trip, set-algebra results that thread source bytes through into         \
     * the destination flex).  Without per-i rebase, flexResize realloc        \
     * freed the buffer and abstractWriteFullEntry → flexWritePayload        \
     * memmove'd from the dangling pointer.  Restricted to FLEX_IS_STR         \
     * because non-string encodings store inline values (no flex pointer).     \
     * Bounded stack arrays: COMMON_INSERT_MAX_CONTENTS_ = 8 covers            \
     * multimap arity up to 5 with headroom on the hot path; wider             \
     * entries (large multimap arity, wide flexAppendMultiple) spill to        \
     * a single heap allocation that frees itself at scope exit. */            \
    enum { COMMON_INSERT_MAX_CONTENTS_ = 8 };                                  \
    int64_t commonFlexSelfOffsetStack_[COMMON_INSERT_MAX_CONTENTS_] = {0};     \
    bool commonFlexSelfBoolStack_[2 * COMMON_INSERT_MAX_CONTENTS_] = {0};      \
    int64_t *commonFlexSelfOffset_ = commonFlexSelfOffsetStack_;               \
    bool *commonFlexSelfAfterFe_ = commonFlexSelfBoolStack_;                   \
    bool *commonFlexSelfAlias_ =                                               \
        commonFlexSelfBoolStack_ + COMMON_INSERT_MAX_CONTENTS_;                \
    cleaner(zfree) void *commonFlexSelfHeap_ = NULL;                           \
    if (contentsCount > COMMON_INSERT_MAX_CONTENTS_) {                         \
        commonFlexSelfHeap_ =                                                  \
            zcalloc(contentsCount, sizeof(int64_t) + 2 * sizeof(bool));        \
        commonFlexSelfOffset_ = (int64_t *)commonFlexSelfHeap_;                \
        commonFlexSelfAfterFe_ =                                               \
            (bool *)(commonFlexSelfOffset_ + contentsCount);                   \
        commonFlexSelfAlias_ = commonFlexSelfAfterFe_ + contentsCount;         \
    }                                                                          \
    for (uint_fast32_t commonFlex_i_ = (contentsStartOffset);                  \
         commonFlex_i_ < contentsCount; commonFlex_i_++) {                     \
        const uint8_t *commonFlex_d_ = contents[commonFlex_i_]->data;          \
        if (FLEX_IS_STR(contents[commonFlex_i_]->encoding) &&                  \
            commonFlex_d_ > (const uint8_t *)f &&                              \
            commonFlex_d_ < ((const uint8_t *)f + flexSize)) {                 \
            commonFlexSelfAlias_[commonFlex_i_] = true;                        \
            commonFlexSelfOffset_[commonFlex_i_] =                             \
                commonFlex_d_ - (const uint8_t *)f;                            \
            commonFlexSelfAfterFe_[commonFlex_i_] =                            \
                commonFlex_d_ > (const uint8_t *)fe;                           \
        }                                                                      \
    }                                                                          \
                                                                               \
    /* 'insertSize' is the sum of three things:                                \
     *   - size of data length (or the encoded type byte)                      \
     *   - actual size of the data.                                            \
     *   - size of data length again (or the encoded type byte again) */       \
    int64_t insertSize = 0;                                                    \
    for (size_t i = contentsStartOffset; i < contentsCount; i++) {             \
        insertSize += abstractInsertSizeFromInsertContents(contents[i]);       \
    }                                                                          \
                                                                               \
    /* Now 'insertSize' is: [(ENCODING SIZE * (2 or 1)) + DATA SIZE] for       \
     * each element being added in this insert. */                             \
    int64_t reallocSize = insertSize;                                          \
                                                                               \
    /* this dumb while(0) is so we can put a semicolon after the macro call */ \
    do {                                                                       \
    } while (0)

#define commonFlexInsertWriteData_(contentsStartOffset)                        \
    /* If caller requested header delta details, populate said details. */     \
    if (headerChangedBy) {                                                     \
        headerChangedBy->insertedBytes = reallocSize;                          \
        headerChangedBy->headerDiff = inserted;                                \
    }                                                                          \
                                                                               \
    /* After resize()/realloc(), restore original pointer offsets. */          \
    if (copyFromSelf) {                                                        \
        data = f + offsetData;                                                 \
        const bool feEncodingOverwritesData =                                  \
            (fe + contents[0]->encodingLen) > data;                            \
        void *properDataAfterNewEncoding = fe + contents[0]->encodingLen;      \
        if (feEncodingOverwritesData && data != properDataAfterNewEncoding) {  \
            /* If our new encoding length would overwrite existing data during \
             * a replace, we need to move the current data up past the new     \
             * encoding write position (or else writing the new encoding will  \
             * write over some of the initial bytes of our data). */           \
            memmove(properDataAfterNewEncoding, data, contents[0]->len);       \
            contents[0]->data = properDataAfterNewEncoding;                    \
            /* flexWritePayload() does a simple test to check if the target of \
             * the write is the same as the source data.  If so, it doesn't    \
             * bother copying anything because the target pointer matches the  \
             * source data pointer. */                                         \
        } else {                                                               \
            if (dataIsAfterFe && !feIsEntryHoldingData) {                      \
                /* If 'data' is after 'fe' (and 'fe' is NOT 'data'),           \
                 * we just moved 'fe' by 'reallocSize', so we must             \
                 * also move 'data' by 'reallocSize'.                          \
                 * else, if 'data' is before 'fe', moving 'fe' has no          \
                 * impact on 'data'                                            \
                 */                                                            \
                data += reallocSize;                                           \
            }                                                                  \
                                                                               \
            contents[0]->data = data;                                          \
        }                                                                      \
    }                                                                          \
                                                                               \
    /* If we are *replacing*, 'contentsStartOffset' is the offset into         \
     * contents[] where we want to start writing (because we previously        \
     * moved 'fe' up by replacement skip slots).                               \
     * If we are *not* replacing, then 'contentsStartOffset' is 0              \
     * and we consume the entire contents[]. */                                \
    for (uint_fast32_t i = (contentsStartOffset); i < contentsCount; i++) {    \
        /* Per-i self-aliasing rebase: when contents[i]->data pointed          \
         * inside this flex before realloc, restore it relative to the         \
         * post-realloc base.  contents[0] in the non-replace path was         \
         * already restored by the copyFromSelf branch above; the per-i        \
         * rebase here covers the replace path's contents[1..N-1] (and is      \
         * a no-op when no aliasing was detected).  See the                    \
         * commonFlexInsertCalculatePreconditions_ comment for why.            \
         * The reallocSize bump for "after-fe" sources matches the existing    \
         * contents[0] rebase at line ~1737 because the same memmove that      \
         * shifted fe also shifted post-fe bytes. */                           \
        if (commonFlexSelfAlias_[i] &&                                         \
            !(i == 0 && (contentsStartOffset) == 0)) {                         \
            contents[i]->data =                                                \
                (const uint8_t *)f + commonFlexSelfOffset_[i] +                \
                (commonFlexSelfAfterFe_[i] ? (int64_t)reallocSize : 0);        \
        }                                                                      \
        abstractWriteFullEntry(&fe, contents[i]);                              \
    }                                                                          \
                                                                               \
    /* commonFlexSelfHeap_ frees itself at scope exit (cleaner) */             \
                                                                               \
    /* dumb while(0) so we can put a semicolon after macro */                  \
    do {                                                                       \
    } while (0)

/* ====================================================================
 * INSERT OPTIMIZED FOR INSERT
 * ==================================================================== */
/* Insert 'contentsCount' elements of contents[] starting 'fe' while optionally
 * returning insert metadata in 'headerChangedBy'. */
DK_STATIC void flexInsert_____(flex **const ff, flexEntry *fe,
                               flexInsertContents *contents[],
                               const uint_fast32_t contentsCount,
                               flexHeaderInfo *const headerChangedBy) {
    flex *f = *ff;

    commonFlexInsertCalculatePreconditions_(0);

    ssize_t offset = fe - f;
    /* Step 1: grow flex for new entry; restore metadata */
    /* GROWING.  REALLOC then MEMMOVE */
    const int_fast8_t inserted =
        flexResize(ff, flexSize + reallocSize, contentsCount);
    offset += inserted;
    offsetData += inserted;
    flexSize += inserted;
    f = *ff;
    fe = f + offset;

    /* Step 2: open a "data hole" to make room for the new entry. */
    /* Open up hole in flex for new entry */
    /* Before:  [A][B][P][C][D]
     * After:   [A][B][NEW][P][C][D].
     * Move all data from fe[0] to fe[flexSize-offset] after
     * the size of our new element. */
    memmove(fe + insertSize, fe, flexSize - offset);

    /* Step 3: write new entry */
    commonFlexInsertWriteData_(0);
}

/* ====================================================================
 * INSERT OPTIMIZED FOR REPLACE
 * ==================================================================== */
/* Replace 'contentsCount - replaceOffset' elements of contents[] starting
 * 'p + replaceOffset' while optionally returning insert metadata
 * in 'headerChangedBy'. */
/* replaceOffset is the offset into 'contents' where we *start* replacing.
 * If replaceOffset == 0, replace exactly 'contentsCount' elements
 *                        starting by writing contents[0] to 'fe', ...
 * If replaceOffset == 1, replace exactly 'contentsCount - 1' elements
 *                        starting by writing contents[1] to 'p+1', ... */
/* replaceOffset can also be read as:
 *   "number of elements to skip in contents[] before inserting"
 * - as well as -
 *   "number of times to advance 'fe' before beginning to insert." */
DK_STATIC void flexInsertReplace_____(flex **const ff, flexEntry *fe,
                                      flexInsertContents **const contents,
                                      const uint_fast32_t contentsCount,
                                      const uint_fast32_t replaceOffset,
                                      flexHeaderInfo *const headerChangedBy) {
    assert(contentsCount > replaceOffset);

    flex *f = *ff;

    /* If replacement is requested but we have no element to replace,
     * revert to a regular insert-at-position. */
    if (fe == GET_END(f)) {
        flexInsert_____(ff, fe, contents, contentsCount, headerChangedBy);
        return;
    }

    commonFlexInsertCalculatePreconditions_(replaceOffset);

    /* Calculate size of entires we are about to replace. */
    /* replacingElements is the number of elements in contents[]
     * being replaced, so subtract the initial offset non-replace
     * elements. */
    int_fast32_t replacingElements = contentsCount - replaceOffset;
    int_fast32_t replacingFEOffset = replaceOffset;

    /* Move 'fe' up to our first *replace* offset.
     * (e.g. if contents[0] is a key but we are replacing only values,
     *       skip over the key and obtain our first value to
     *       start replacing). */
    /* NOTE: if you abuse the interface and try to replace a 2-arity
     * map with a not-that-arity map, you could do bad things like running
     * beyond the end of the flex.  We don't check for misuse because
     * abusing the interface isn't our problem at the moment. */
    while (replacingFEOffset--) {
        fe += flexRawEntryLength(fe);
    }

    /* Now walk the remainder of our target replace elements
     * and calculate their total replacement size. */
    /* If existing entries sum smaller than replace elements, flex grows.
     * If existing entries sum larger than replace elements, flex shrinks. */
    flexEntry *walkerP = fe;
    size_t pEntryTotalSize = 0;
    while (replacingElements--) {
        const size_t entrySize = flexRawEntryLength(walkerP);
        reallocSize -= entrySize;
        pEntryTotalSize += entrySize;
        walkerP += entrySize;
    }

    /* This is slightly ugly and copy/pasty because we have to do the same
     * operations in different orders depending on
     * growing (resize *then* move)
     *  - versus -
     * shrinking (move *then* resize). */
    int_fast8_t inserted = 0;
    if (reallocSize != 0) {
        /* Store offset because a realloc may change the address of f. */
        ssize_t offset = fe - f;
        if (reallocSize > 0) {
            /* GROWING.  REALLOC then MEMMOVE */
            /* Step 1: grow flex for new entry; restore metadata */
            /* Note: This is a *replace* so we don't increase our count.
             *       If you do bad things like replace entries of a
             *       elementsPerEntry(2) map with elementsPerEntry(3) or
             *       elementsPerEntry(1), everything will break horribly
             *       for you. */
            inserted = flexResize(ff, flexSize + reallocSize, 0);
            offset += inserted;
            offsetData += inserted;
            flexSize += inserted;
            f = *ff;
            fe = f + offset;

            /* Step 2: open a "data hole" to make room for the new entry. */
            /* Before:  [A][B][PP][C][D]
             * After:   [A][B][PPPP][C][D]
             * Move all data from c[0] to fe[flexSize - offset]
             * to exist after fe[insertSize] */
            const flexEntry *next = fe + pEntryTotalSize;
            const size_t moveNextEntryUpBy = flexSize - (next - f);
            if (moveNextEntryUpBy > 0) {
                /* Move entries *after* fe to make room for new entry. */
                memmove(fe + insertSize, next, moveNextEntryUpBy);
            }
        } else {
            /* SHRINKING.  memmove THEN realloc. */
            /* Before:  [A][B][PP][C][D]
             * After:   [A][B][P][C][D] (smaller than before, fe got smaller) */

            /* Step 1: move data down to new shrunk positions. */
            const flexEntry *next = fe + pEntryTotalSize;
            const size_t moveNextEntryDownBy = flexSize - (next - f);

            /* Step 1a: if shrink is for the current position, we need
             *          more accounting to retain data properly if
             *          the encoding length of the current position is
             *          also shrinking. */
            const bool resizingPShrinksData =
                (fe + contents[0]->encodingLen) < data;
            if (feIsEntryHoldingData && resizingPShrinksData) {
                /* If we are updating from our own data and our encoding
                 * is shrinking, move data down to match up to the new
                 * shorter encoding length.
                 * Because the encoding for 'fe' is shrinking, we move 'fe' down
                 * by the new encoding size to retain proper initial data. */
                memmove(fe + contents[0]->encodingLen, data, contents[0]->len);
                /* data = fe + contents[0]->encodingLen; // never read */
            }

            /* Step 1b: normal case: move entries after the new size of 'fe'
             *          down to match the new size of 'fe' */
            if (moveNextEntryDownBy > 0) {
                /* Move entries *after* fe down to close shrink resize gap. */
                memmove(fe + insertSize, next, moveNextEntryDownBy);
            }

            /* Step 2: realloc flex to new smaller size; restore metadata */
            /* Don't increase element count because this is a *replace* */
            inserted = flexResize(ff, flexSize + reallocSize, 0);
            offset += inserted;
            offsetData += inserted;
            f = *ff;
            fe = f + offset;
        }
    }

    /* Replace data using:
     *   contents[replaceOffset] up to contents[elementsPerEntry - 1] */
    commonFlexInsertWriteData_(replaceOffset);
}

DK_INLINE_ALWAYS void
flexResizeEntry____(flex **const ff, flexEntry *const fe, size_t newLenForEntry,
                    flexHeaderInfo *const headerChangedBy) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);

    flexInsertContents content = {
        .encoding = 0, /* encoding == 0 means string/blob type */
        .data = FLEX_ENTRY_DATA(&entry), /* copy existing data to new size */
        .len = newLenForEntry}; /* use new len instead of current len */
    flexInsertContents *contents[1] = {&content};

    /* If entry grows larger (i.e. gets more space allocated for it),
     * the new space larger than the current size has arbitrary contents */
    flexInsertReplace_____(ff, fe, contents, 1, 0, headerChangedBy);
}

void flexResizeEntry(flex **const ff, flexEntry *const fe,
                     size_t newLenForEntry) {
    flexResizeEntry____(ff, fe, newLenForEntry, NULL);
}

DK_INLINE_ALWAYS void flexInsert____(flex **const ff, flexEntry *const fe,
                                     const flexEncoding encoding,
                                     const void *const data, const size_t len,
                                     const bool replacingFE,
                                     flexHeaderInfo *const headerChangedBy) {
    flexInsertContents content = {
        .encoding = encoding, .data = data, .len = len};
    flexInsertContents *contents[1] = {&content};

    if (replacingFE) {
        /* 0 below because we replace the *entire* element starting at 'fe' */
        flexInsertReplace_____(ff, fe, contents, 1, 0, headerChangedBy);
    } else {
        flexInsert_____(ff, fe, contents, 1, headerChangedBy);
    }
}

void flexInsertSigned(flex **const ff, flexEntry *const fe, int64_t i) {
    const flexEncoding encoding = flexEncodingSigned(i);
    i = flexPrepareSigned(i);
    flexInsert_(ff, fe, encoding, &i, sizeof(i));
}

void flexInsertUnsigned(flex **const ff, flexEntry *const fe, uint64_t u) {
    const flexEncoding encoding = flexEncodingUnsigned(u);
    flexInsert_(ff, fe, encoding, &u, sizeof(u));
}

void flexInsertFloat16(flex **const ff, flexEntry *const fe, float value) {
    /* Force half float encoding regardless of value truncation */
    flexInsert_(ff, fe, FLEX_REAL_16B, &value, sizeof(value));
}

void flexInsertFloatB16(flex **const ff, flexEntry *const fe, float value) {
    /* Force half float encoding regardless of value truncation */
    flexInsert_(ff, fe, FLEX_REAL_B16B, &value, sizeof(value));
}

void flexInsertFloat(flex **const ff, flexEntry *const fe, float value) {
    const flexEncoding encoding = flexEncodingFloat(value);
    flexInsert_(ff, fe, encoding, &value, sizeof(value));
}

void flexInsertDouble(flex **const ff, flexEntry *const fe, double dvalue) {
    const flexEncoding encoding = flexEncodingDouble(dvalue);

    if (encoding == FLEX_REAL_64B) {
        flexInsert_(ff, fe, encoding, &dvalue, sizeof(dvalue));
    } else {
        float fvalue = dvalue;
        flexInsert_(ff, fe, encoding, &fvalue, sizeof(fvalue));
    }
}

void flexInsertTrue(flex **const ff, flexEntry *const fe, void *none) {
    DK_NOTUSED(none);
    flexEncoding encoding = FLEX_TRUE;
    flexInsert_(ff, fe, encoding, NULL, 0);
}

void flexInsertFalse(flex **const ff, flexEntry *const fe, void *none) {
    DK_NOTUSED(none);
    flexEncoding encoding = FLEX_FALSE;
    flexInsert_(ff, fe, encoding, NULL, 0);
}

void flexInsertNull(flex **const ff, flexEntry *const fe, void *none) {
    DK_NOTUSED(none);
    flexEncoding encoding = FLEX_NULL;
    flexInsert_(ff, fe, encoding, NULL, 0);
}

/* Stores a numeral as its canonical number (databoxCanonical: a 64-bit
 * integer, float, double or 128-bit integer), and anything else as bytes. */
void flexInsertBytes(flex **const ff, flexEntry *const fe, const void *data,
                     size_t len) {
/* 'fe' MUST be inside the bounds of 'f' but not be f itself. */
#if FLEX_DEBUG_EXTENSIVE
    assert(fe != *ff);
    assert(fe && *ff);
    assert(fe > *ff);
    assert(fe <= (*ff + flexTotalBytes_(*ff)));
#endif

    const databox bytes = {
        .data.bytes.custart = data, .type = DATABOX_BYTES, .len = len};
    databoxBig scratch;
    const databox *const canonical = databoxCanonical(&bytes, &scratch);
    if (canonical != &bytes) {
        flexInsertByType(ff, fe, canonical);
    } else {
        flexInsert_(ff, fe, FLEX_STRING_ENCODING(len), data, len);
    }
}

DK_STATIC void flexInsertByType_(flex **const ff, flexEntry *const fe,
                                 const databox *box) {
    /* Special case: if databox is NULL, insert NULL encoding. */
    if (!box) {
        flexInsert_(ff, fe, FLEX_NULL, NULL, 0);
        return;
    }

    switch (box->type) {
    case DATABOX_BYTES:
        flexInsert_(ff, fe, FLEX_STRING_ENCODING(box->len),
                    box->data.bytes.start, databoxLen(box));
        break;
    case DATABOX_BYTES_EMBED:
        flexInsert_(ff, fe, FLEX_STRING_ENCODING(box->len),
                    box->data.bytes.embed, databoxLen(box));
        break;
    case DATABOX_SIGNED_64: {
        /* Choose the encoding from the original value, then store the
         * offset payload (same order as flexInsertSigned). */
        const flexEncoding encoding = flexEncodingSigned(box->data.i);
        const intmax_t i = flexPrepareSigned(box->data.i);
        flexInsert_(ff, fe, encoding, &i, sizeof(i));
        break;
    }
    case DATABOX_UNSIGNED_64:
    case DATABOX_PTR:
        /* No native pointer encoding: a pointer is stored as an integer,
         * as in insertContentsFromBox. */
        flexInsert_(ff, fe, flexEncodingUnsigned(box->data.u), &box->data.u,
                    sizeof(box->data.u));
        break;
    case DATABOX_CONTAINER_REFERENCE_EXTERNAL: {
        /* Same encoding as insertContentsFromBox. */
        const uint64_t encoded =
            flexEncodingReferenceUnsignedEncode(box->data.u64);
        flexInsert_(ff, fe, flexEncodingReferenceUnsigned(box->data.u64),
                    &encoded, sizeof(encoded));
        break;
    }
#if FLEX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC: {
        /* Same encoding as insertContentsFromBox: the pointer itself, in 6
         * bytes when its top 16 bits are clear. */
        const uint64_t ptr = (uintptr_t)box->data.ptr;
        const bool fits48 = ptr < (UINT64_C(1) << 48);
        flexInsert_(ff, fe,
                    fits48 ? FLEX_EXTERNAL_MDSC_48B : FLEX_EXTERNAL_MDSC_64B,
                    &ptr, fits48 ? 48 / 8 : 64 / 8);
        break;
    }
#endif
    case DATABOX_SIGNED_128: {
        databoxWidePayloadRequire(box);
        assert(box->big);
        /* Same order as the 64-bit arm. */
        const flexEncoding encoding = flexEncodingSignedBig(*box->data.i128);
        const __int128_t add = flexPrepareSignedBig(*box->data.i128);
        flexInsert_(ff, fe, encoding, &add, sizeof(add));
        break;
    }
    case DATABOX_UNSIGNED_128:
        databoxWidePayloadRequire(box);
        assert(box->big);
        flexInsert_(ff, fe, flexEncodingUnsignedBig(*box->data.u128),
                    box->data.u128, sizeof(*box->data.u128));
        break;
    case DATABOX_FLOAT_32:
        flexInsert_(ff, fe, flexEncodingFloat(box->data.f32), &box->data.f32,
                    sizeof(box->data.f32));
        break;
    case DATABOX_DOUBLE_64: {
        const flexEncoding encodeAs = flexEncodingDouble(box->data.d64);

        /* Encode requires all 64 bits, so encode as float64 */
        if (encodeAs == FLEX_REAL_64B) {
            flexInsert_(ff, fe, FLEX_REAL_64B, &box->data.d64,
                        sizeof(box->data.d64));
        } else {
            /* else, encoding can be a float32, float16, or bfloat16 */
            const float sender = box->data.d64;
            flexInsert_(ff, fe, encodeAs, &sender, sizeof(sender));
        }

        break;
    }
    case DATABOX_TRUE:
        flexInsert_(ff, fe, FLEX_TRUE, NULL, 0);
        break;
    case DATABOX_FALSE:
        flexInsert_(ff, fe, FLEX_FALSE, NULL, 0);
        break;
    case DATABOX_NULL:
        flexInsert_(ff, fe, FLEX_NULL, NULL, 0);
        break;
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE: {
        /* Nested flex, stored whole; same encoding as insertContentsFromBox.
         * The entry is sized from the nested flex's own header, never from
         * 'len': flexGetByType reports a container's len with its reverse
         * trailer included, so a box read back from a flex carries a len
         * larger than the nested flex, and the insert reserves space from
         * the header. */
        const flex *const inner = box->data.bytes.start;
        flexInsert_(ff, fe,
                    FLEX_CONTAINER_MAP + DATABOX_CONTAINER_OFFSET(box->type),
                    inner, flexBytes(inner));
        break;
    }
    default:
        assert(NULL);
        __builtin_unreachable();
    }
}

typedef enum conversionOverride {
    CONVERSION_OVERRIDE_NONE = 0,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT6 = 6,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT7 = 7,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT8 = 8,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT9 = 9,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT10 = 10,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT11 = 11,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT12 = 12,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT13 = 13,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT14 = 14,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT15 = 15,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT16 = 16,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT17 = 17,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT18 = 18,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT19 = 19,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT20 = 20,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT21 = 21,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT22 = 22,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT23 = 23,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT24 = 24,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT25 = 25,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT26 = 26,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT27 = 27,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT28 = 28,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT29 = 29,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT30 = 30,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT31 = 31,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT32 = 32,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT33 = 33,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT34 = 34,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT35 = 35,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT36 = 36,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT37 = 37,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT38 = 38,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT39 = 39,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT40 = 40,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT41 = 41,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT42 = 42,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT43 = 43,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT44 = 44,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT45 = 45,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT46 = 46,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT47 = 47,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT48 = 48,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT49 = 49,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT50 = 50,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT51 = 51,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT52 = 52,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT53 = 53,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT54 = 54,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT55 = 55,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT56 = 56,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT57 = 57,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT58 = 58,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT59 = 59,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT60 = 60,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT61 = 61,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT62 = 62,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT63 = 63,
    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT64 = 64,
} conversionOverride;

/* Note: this 'box' is MUTATED and does NOT retain the original
 *       value in some cases due to how we encode our inserts. */
/* Also, disable ubsan complaining about our float to integer conversion
 * tests when trying to minimize float/double storage. */
/* Returns true if conversion was applied.
 * Returns false if no conversion applied. */
__attribute__((no_sanitize("float-cast-overflow"))) DK_STATIC bool
insertContentsFromBox(databox *box, flexInsertContents *c,
                      const conversionOverride override, const size_t idx,
                      __int128_t *scratchBuf) {
    static const flexInsertContents cNull = {0};
    *c = cNull;

    switch (box->type) {
    case DATABOX_BYTES_VOID:
        c->isVoidEntry = true;
        /* fallthrough */
    case DATABOX_BYTES_EMBED:
        c->data = box->data.bytes.embed;
        c->len = box->len;
        c->encoding = FLEX_STRING_ENCODING(box->len);
        break;
    case DATABOX_BYTES:
#if FLEX_ENABLE_PTR_MDSC
        if (override && box->len > override) {
            assert(!box->allocated);
            /* @own: container; free=mdscfree */
            mdsc *const converted = mdscnewlen(box->data.bytes.start, box->len);
            box->data.ptr = converted;
            box->type = DATABOX_PTR_MDSC;
            goto resumeMDSCInsert;
        }
#endif
        c->data = box->data.bytes.start;
        c->encoding = FLEX_STRING_ENCODING(box->len);
        c->len = box->len;
#if FLEX_ENABLE_STRING_DANGEROUS
        assert(c->len > 0);
#endif
        /* NB: We can't store 0-length strings, potentially use auto-empty. */
        break;
    case DATABOX_SIGNED_64:
#if 0
    becomeSigned:
#endif
        c->encoding = flexEncodingSigned(box->data.i64);
        box->data.i64 = flexPrepareSigned(box->data.i64);
        c->data = (int64_t *)&box->data.i64;
        c->len = sizeof(box->data.i64);
        break;
    case DATABOX_PTR:
        /* NOTE: We don't have a native FLEX_PTR type,
         *       so we just store as an integer.
         * We expect the user to use the retrieved value
         * properly.
         * Of course, persisting pointers to storage and
         * restoring them won't have desired effects unless
         * you calculate offsets against a common base. */
        /* fallthrough */
    case DATABOX_UNSIGNED_64:
#if 0
    becomeUnsigned:
#endif
        c->encoding = flexEncodingUnsigned(box->data.u64);
        c->data = (uint64_t *)&box->data.u64;
        c->len = sizeof(box->data.u64);
        break;
    case DATABOX_SIGNED_128:
        databoxWidePayloadRequire(box);
        assert(box->big);
        /* Note: we MUST NOT modify the i128 inside 'box' because it
         *       STILL points to the ORIGINAL i128.
         *       We did not copy the value to get here.
         *       But we *do* have scratch space input to this function,
         *       so we use the scratch space to store our altered value,
         *       *then* we just point our temporary copyBox ('box') to the
         *       scratch space for future reading.
         *       The scratch space will be cleaned up external to us here. */
        /* MAYBE TODO:
         *   - we could get rid of all these scratch allocations if we moved
         *     'flexPrepareSigned[Big] down to the actual insert function.
         *     But, the insert function would probably need to reconstruct
         *     the data, do its subtraction, then reassemble the data back
         *     into bytes for the insert, *BUT* it would be much cleaner
         *     (and less trouble on allocators/stack) doing it there instead
         *     of needing to drag "scratch space" into here all the time. */
        c->encoding = flexEncodingSignedBig(*box->data.i128);
        scratchBuf[idx] = flexPrepareSignedBig(*box->data.i128);
        box->data.i128 = &scratchBuf[idx];
        c->data = box->data.i128;
        c->len = sizeof(*box->data.i128);
        break;
    case DATABOX_UNSIGNED_128:
        databoxWidePayloadRequire(box);
        assert(box->big);
        c->encoding = flexEncodingUnsignedBig(*box->data.u128);
        c->data = box->data.u128;
        c->len = sizeof(*box->data.u128);
        break;
    case DATABOX_FLOAT_32: {
        const float floater = box->data.f32;

#if 0
        /* Okay, so these _were_ a good idea except for the fact
         * our upstream is converting user strings to floats (fine),
         * but then if we convert the floats to integers, we can't
         * round-trip the user data.
         *
         * e.g. input string "3.0" becomes float 3.0 becomes integer 3
         *
         * So, now we don't automatically de-convert floats to integers
         * in flex encodings. Such conversions must be done at a higher
         * level before we re-insert into the flex. */

        /* Attempt to convert float to integer storage first */
        if ((int64_t)floater == floater) {
            box->data.i = floater;
            goto becomeSigned;
        }

        if ((uint64_t)floater == floater) {
            box->data.u = floater;
            goto becomeUnsigned;
        }
#endif

        /* Too big or too small for integers, so use float storage... */
        c->encoding = flexEncodingFloat(floater);
        c->data = (float *)&box->data.f32;
        c->len = sizeof(box->data.f32);
        break;
    }
    case DATABOX_DOUBLE_64: {
        const double floater = box->data.d64;

#if 0
        /* Attempt to convert float to integer storage first */
        if ((int64_t)floater == floater) {
            box->data.i = floater;
            goto becomeSigned;
        }

        if ((uint64_t)floater == floater) {
            box->data.u = floater;
            goto becomeUnsigned;
        }
#endif

        /* Too big or too small for integers, so use float storage... */
        c->encoding = flexEncodingDouble(floater);
        if (c->encoding == FLEX_REAL_64B) {
            c->data = (double *)&box->data.d64;
            c->len = sizeof(box->data.d64);
        } else {
            /* else, type is f32, f16, or bf16 which all
             * take a float as pre-storage representation. */
            /* Because c->encoding != FLEX_REAL_64B here, we are
             * guaranteed we can convert the double64 to a float32
             * with no loss of precision. */
            box->data.f32 = floater;
            c->data = (float *)&box->data.f32;
            c->len = sizeof(box->data.f32);
        }

        break;
    }
#if FLEX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
    resumeMDSCInsert: {
        const size_t len = (uintptr_t)box->data.ptr < (1ULL << 48) ? 6 : 8;
        c->encoding =
            len == 6 ? FLEX_EXTERNAL_MDSC_48B : FLEX_EXTERNAL_MDSC_64B;
        c->data = &box->data.ptr;
        c->len = len;
        return true;
        break;
    }
#endif
    case DATABOX_CONTAINER_REFERENCE_EXTERNAL:
        c->encoding = flexEncodingReferenceUnsigned(box->data.u64);
        box->data.u64 = flexEncodingReferenceUnsignedEncode(box->data.u64);
        c->data = (uint64_t *)&box->data.u64;
        c->len = sizeof(box->data.u64);
        break;
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
        /* Derive flex encoding from box encoding using enum offset math */
        c->encoding = FLEX_CONTAINER_MAP + DATABOX_CONTAINER_OFFSET(box->type);
        c->data = box->data.bytes.start;
        /* Sized from the nested flex's own header, never from 'len': the
         * insert reserves space from that header, and flexGetByType
         * reports a container's len with its reverse trailer included. */
        c->len = flexBytes(c->data);
        break;
    case DATABOX_CONTAINER_CFLEX_MAP:
    case DATABOX_CONTAINER_CFLEX_LIST:
    case DATABOX_CONTAINER_CFLEX_SET:
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        /* TODO: we aren't storing nested compressed maps.
         *       if map is compressed, must uncompress for storage. */
        c->encoding = FLEX_CONTAINER_MAP + DATABOX_CONTAINER_OFFSET(box->type);
        c->data = box->data.bytes.start;
        c->len = cflexBytes(c->data);
        break;
    case DATABOX_TRUE:
        c->encoding = FLEX_TRUE;
        break;
    case DATABOX_FALSE:
        c->encoding = FLEX_FALSE;
        break;
    case DATABOX_NULL:
        c->encoding = FLEX_NULL;
        break;
    default:
        assert(NULL);
        __builtin_unreachable();
    }

    return false;
}

void flexInsertByType(flex **const ff, flexEntry *const fe,
                      const databox *box) {
    flexInsertByType_(ff, fe, (databox *)box);
}

/* ====================================================================
 * flex replace
 * ==================================================================== */
void flexReplaceByType(flex **const ff, flexEntry *const fe,
                       const databox *box) {
    databox copyBox = *box;
    flexInsertContents content = {0};
    __int128_t scratch;
    insertContentsFromBox(&copyBox, &content, CONVERSION_OVERRIDE_NONE, 0,
                          &scratch);
    flexInsertContents *contents[1] = {&content};
    flexInsertReplace_____(ff, fe, contents, 1, 0, NULL);
}

void flexReplaceBytes(flex **const ff, flexEntry *const fe, const void *s,
                      const size_t len) {
    flexInsertReplace_(ff, fe, 0, s, len);
}

DK_STATIC void flexForceReplaceSigned__(flex **const ff, flexEntry *const fe,
                                        const flexEncoding enc,
                                        const int64_t i) {
    /* Note — 'i' *ALREADY* has run through flexPrepareSigned before here. */
    flexInsertReplace_(ff, fe, enc, &i, sizeof(i));
}

DK_STATIC void flexForceReplaceUnsigned__(flex **const ff, flexEntry *const fe,
                                          const uint64_t u) {
    flexInsertReplace_(ff, fe, flexEncodingUnsigned(u), &u, sizeof(u));
}

bool flexReplaceInteger__(flex **const ff, flexEntry *const fe,
                          const databox *box) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);

    /* TODO: Also allow replace of non-integers, but integer-storage types.
     */
    if (!flexEntryIsIntegerNarrow_(fe)) {
        return false;
    }

    databox useBox = *box;
    uint_fast8_t oldEncodingSize =
        EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(entry.encoding);
    uint_fast8_t newEncodingSize;
    flexEncoding enc;
    switch (box->type) {
    case DATABOX_SIGNED_64:
        enc = flexEncodingSigned(useBox.data.i64);
        newEncodingSize = EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(enc);
        useBox.data.i64 = flexPrepareSigned(useBox.data.i64);
        break;
    case DATABOX_UNSIGNED_64:
        enc = flexEncodingUnsigned(useBox.data.u64);
        newEncodingSize = EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(enc);
        break;
    default:
        return false;
    }

    /* If new value fits in the current encoding, just update in-place */
    if (newEncodingSize <= oldEncodingSize) {
        /* If new encoding doesn't shrink our allocation, update in-place.
         *
         * Even if our new value is below the minimum size for a field
         * (e.g. storing '4' in a 32 bit field), we update in-place rather
         * than deleting the old larger allocation and replacing with a new
         * smaller allocation.  Updating values smaller than our current
         * encoding in-place saves us from delete+realloc+create+realloc at
         * the cost of between 1 and 8 bytes overhead depending on
         * the shrinkage.  (e.g. a 64 bit entry shrinking to number '4'
         * still takes 8 bytes, even though we could store '4' as
         * just one byte). */

        /* Save new integer with the _original_ encoding's width, even if
         * that is larger than the optimal encoding for this value.  The
         * sign comes from the new value: a negative value is stored as
         * its magnitude (the negation of its prepared value) under the
         * NEG encoding of that width, so the encoding byte at each end of
         * the entry is rewritten to match. */
        const bool negative =
            ((enc - FLEX_NEG_8B) % FLEX_INTEGER_ENCODING_STEP) == 0;
        const flexEncoding widthBase =
            entry.encoding -
            ((entry.encoding - FLEX_NEG_8B) % FLEX_INTEGER_ENCODING_STEP);
        const flexEncoding inPlace = widthBase + (negative ? 0 : 1);
        const uint64_t stored =
            negative ? (uint64_t)-useBox.data.i64 : useBox.data.u64;
        flexData *const data = FLEX_ENTRY_DATA(&entry);
        varintExternalPutFixedWidthQuick_(data, stored, oldEncodingSize);
        fe[0] = inPlace;
        data[oldEncodingSize] = inPlace;
        return true;
    }

    /* else, new encoding is too big for current entry size.
     *
     * The new value is too large to store in the current encoding, so
     * we must delete the old entry then add a new entry in its place.
     *
     * NOTE: if using middle, the caller must ALSO update their middle or
     *       future searching will break! */
    switch (box->type) {
    case DATABOX_SIGNED_64:
        flexForceReplaceSigned__(ff, fe, enc, useBox.data.i64);
        return true;
    case DATABOX_UNSIGNED_64:
        flexForceReplaceUnsigned__(ff, fe, useBox.data.u64);
        return true;
    default:
        return false;
    }
}

bool flexReplaceSigned(flex **const ff, flexEntry *const fe,
                       const int64_t value) {
    databox box = databoxNewSigned(value);
    return flexReplaceInteger__(ff, fe, &box);
}

bool flexReplaceUnsigned(flex **const ff, flexEntry *const fe,
                         const uint64_t value) {
    databox box = databoxNewUnsigned(value);
    return flexReplaceInteger__(ff, fe, &box);
}

/* ====================================================================
 * flex in-place incrby
 * ==================================================================== */
bool flexIncrbySigned(flex **const ff, flexEntry *const fe,
                      const int64_t incrby, int64_t *newval) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);

    /* Encoding of '0' here probably means you requested to incr
     * on a string with length 1, which is bad... */
    assert(entry.encoding);

    /* This entry point takes a 64-bit delta and reports a 64-bit
     * result, so it operates on values that fit 64 bits (native
     * integers, and booleans promoted to 0/1); a 96/128-bit-stored
     * value can't round-trip through the int64 result and needs a
     * wide increment variant. The gate must precede the load:
     * flexLoadSigned decodes a 96/128-bit entry THROUGH a wide pointer
     * the narrow databox here does not provide, so the entry must be
     * rejected before it is ever loaded. */
    if (!flexEntryIsIntegerNarrow_(fe) && !FLEX_IS_BOOL(entry.encoding)) {
        return false;
    }

    /* Get current value */
    const int64_t value = flexLoadSigned(&entry);

    /* Incrementing by zero succeeds and changes nothing. */
    if (incrby == 0) {
        if (newval) {
            *newval = value;
        }

        return true;
    }

    /* Refuse a result outside int64: no wrap, no saturation. */
    int64_t incremented;
    if (__builtin_add_overflow(value, incrby, &incremented)) {
        return false;
    }
    if (newval) {
        *newval = incremented;
    }

    if (FLEX_IS_INTEGER(entry.encoding)) {
        return flexReplaceSigned(ff, fe, incremented);
    }

    /* Allow incrementing and decrementing from an initial boolean condition.
     * This operation replaces the boolean value with an incremented or
     * decremented integer value. */
    if (FLEX_IS_BOOL(entry.encoding)) {
        uint_fast8_t newEncodingSize;
        flexEncoding enc;
        if (incremented >= 0) {
            enc = flexEncodingUnsigned(incremented);
            newEncodingSize = EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(enc);
        } else {
            enc = flexEncodingSigned(incremented);
            newEncodingSize = EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(enc);
            incremented = flexPrepareSigned(incremented);
        }

        flexInsertReplace_(ff, fe, enc, &incremented, newEncodingSize);
        return true;
    }

    assert(NULL && "Attempted to increment something weird?");
    return false;
}

bool flexIncrbyUnsigned(flex **const ff, flexEntry *const fe,
                        const int64_t incrby, uint64_t *newval) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    if (!flexEntryIsIntegerNarrow_(fe)) {
        return false;
    }

    /* Get current value */
    uint64_t value = flexLoadUnsigned(&entry);

    /* Incrementing by zero succeeds and changes nothing. */
    if (incrby == 0) {
        if (newval) {
            *newval = value;
        }

        return true;
    }

    /* Reject wrap: incrby is signed while value and the result are
     * unsigned, so a negative delta larger than value would wrap to a
     * huge number and a positive delta past UINT64_MAX to a tiny one. */
    if (incrby > 0) {
        if ((uint64_t)incrby > UINT64_MAX - value) {
            return false;
        }
    } else {
        const uint64_t magnitude = (incrby == INT64_MIN)
                                       ? ((uint64_t)INT64_MAX + 1)
                                       : (uint64_t)(-incrby);
        if (magnitude > value) {
            return false;
        }
    }

    const uint64_t incremented = value + (uint64_t)incrby;

    if (newval) {
        *newval = incremented;
    }

    return flexReplaceUnsigned(ff, fe, incremented);
}

flex *flexDuplicate(const flex *f) {
    const size_t len = flexTotalBytes_(f);
    /* @own: caller; free=flexFree */
    flex *ret = zcalloc(1, len);
    memcpy(ret, f, len);
    return ret;
}

/* ====================================================================
 * flex splitting
 * ==================================================================== */
/* Split flex '*ff' into two halfs: [0, fe) and [fe, tail].
 * Returns new [fe, tail] flex while modifying '*ff' to be [0, fe). */
flex *flexSplitMiddle(flex **const ff, uint_fast32_t elementsPerEntry,
                      const flexEntry *middleEntry) {
    flex *f = *ff;
    const uint_fast32_t countValues = flexCount_(f) / elementsPerEntry;
    const size_t totalBytes = flexTotalBytes_(f);
    const bool countIsEven = countValues % 2 == 0;

    const uint_fast32_t half = (countValues / 2);
    const uint_fast32_t firstHalfCount = half * elementsPerEntry;
    const uint_fast32_t secondHalfCount =
        (countIsEven ? half : half + 1) * elementsPerEntry;

#ifdef FLEX_DEBUG_EXTENSIVE
    assert(firstHalfCount + secondHalfCount == flexCount_(f));
    assert(middleEntry == flexMiddle(f, elementsPerEntry));
#endif

    /* @own: caller; free=flexFree */
    flex *secondHalf = flexNew();
    const size_t offset = middleEntry - f;
    const size_t firstHalfSize = offset; /* includes first half header */
    const size_t secondHalfSize = totalBytes - offset; /* data size only */

    /* Original (even):
     *   [A, B, C, D, E, F] -> [A, B, C]; [D, E, F]
     * Original (odd):
     *   [A, B, C, D, E, F, G] -> [A, B, C]; [D, E, F, G].
     * We use the input flex for the lower half so we don't have to
     * copy any memory for it, we just truncate the existing list.  The
     * "after" list gets a copy of the top 50% of the input list. */

    /* Copy [Middle, End] into secondHalf */
    flexBulkAppend(&secondHalf, middleEntry, secondHalfSize, secondHalfCount);

    /* *ff = [Head, Middle) */
    flexSetTotalBytesCount(ff, firstHalfSize, firstHalfCount);

#ifdef FLEX_DEBUG_EXTENSIVE
    assert(flexHead(secondHalf));
    assert(flexHead(*ff));
    assert(flexTail(secondHalf));
    assert(flexTail(*ff));
    assert(flexCount_(*ff) + flexCount_(secondHalf) ==
           (countValues * elementsPerEntry));
#endif

    return secondHalf;
}

flex *flexSplit(flex **const ff, const uint_fast32_t elementsPerEntry) {
    return flexSplitMiddle(ff, elementsPerEntry,
                           flexMiddle(*ff, elementsPerEntry));
}

/* ====================================================================
 * flex merging
 * ==================================================================== */
/* Merge flexes 'first' and 'second' by appending 'second' to 'first'.
 *
 * NOTE: The larger flex is reallocated to contain the new merged flex.
 * Either 'first' or 'second' can be used for the result.  The parameter not
 * used will be free'd and set to NULL.
 *
 * After calling this function, the input parameters are no longer valid
 * since they are changed and free'd in-place.
 *
 * The result flex is the contents of 'first' followed by 'second'.
 *
 * On failure: returns NULL if the merge is impossible.
 * On success: returns the merged flex (which is expanded version of either
 * 'first' or 'second', also frees the other unused input flex, and sets the
 * input flex argument equal to newly reallocated flex return value. */
flex *flexMerge(flex **first, flex **second) {
    /* If any params are null, we can't merge, so NULL. */
    if (first == NULL || *first == NULL || second == NULL || *second == NULL) {
        return NULL;
    }

    /* Can't merge same list into itself. */
    if (*first == *second) {
        return NULL;
    }

    const size_t firstBytes = flexTotalBytes_(*first);
    const size_t secondBytes = flexTotalBytes_(*second);

    const size_t firstCount = flexCount_(*first);
    const size_t secondCount = flexCount_(*second);

    const uint_fast8_t firstSize = FLEX_HEADER_SIZE(*first);
    const uint_fast8_t secondSize = FLEX_HEADER_SIZE(*second);

    bool appendToTarget;
    flex *source, *target;
    size_t targetBytes, sourceBytes;
    uint_fast8_t targetSize, sourceSize;
    /* Pick the largest flex so we can resize easily in-place.
     * We must also track if we are now appending or prepending to
     * the target flex. */
    if (firstBytes >= secondBytes) {
        /* retain first, append second to first. */
        target = *first;
        targetBytes = firstBytes;
        targetSize = firstSize;
        source = *second;
        sourceBytes = secondBytes;
        sourceSize = secondSize;
        appendToTarget = true;
    } else {
        /* else, retain second, prepend first to second. */
        target = *second;
        targetBytes = secondBytes;
        targetSize = secondSize;
        source = *first;
        sourceBytes = firstBytes;
        sourceSize = firstSize;
        appendToTarget = false;
    }

    /* Calculate final data bytes (remove all headers and one end marker,
     * add back later) */
    const size_t fbytes = firstBytes + secondBytes - sourceSize;

    /* Same as above for bytes, but now for count too.  */
    const size_t fcount = firstCount + secondCount;

    /* Extend target to new fbytes then append or prepend source. */
    /* @own: caller; free=flexFree */
    target = zrealloc(target, fbytes);
    if (appendToTarget) {
        /* Copy source after target
         *   [TARGET, SOURCE - HEADER] */
        memcpy(target + targetBytes, source + sourceSize,
               sourceBytes - sourceSize);
    } else {
        /* !append == prepending to target */
        /* Move target *contents* exactly size of (source)
         * then copy source into vacataed space (source)
         *   [SOURCE, TARGET - HEADER] */
        memmove(target + sourceBytes, target + targetSize,
                targetBytes - targetSize);
        memcpy(target, source, sourceBytes);
    }

    /* Update header metadata. */
    (void)flexSetTotalBytesCount(&target, fbytes, fcount);

    /* Now free and NULL out what we didn't realloc */
    if (appendToTarget) {
        zfree(*second);
        *second = NULL;
        *first = target;
    } else {
        zfree(*first);
        *first = NULL;
        *second = target;
    }

    return target;
}

/* ====================================================================
 * flex push
 * ==================================================================== */
/* Note: FLEX_ENTRY_END is correct because it's the position *after*
 *       FLEX_ENTRY_TAIL but before FLEX_END. */
#define flexPush_(fun, ...)                                                    \
    do {                                                                       \
        flexEntry *fe = (where == FLEX_ENDPOINT_HEAD)                          \
                            ? FLEX_ENTRY_HEAD(*ff)                             \
                            : FLEX_ENTRY_AFTER_TAIL(*ff);                      \
        (fun)(ff, fe, __VA_ARGS__);                                            \
    } while (0)

void flexPushSigned(flex **const ff, const int64_t i, flexEndpoint where) {
    flexPush_(flexInsertSigned, i);
}

void flexPushUnsigned(flex **const ff, const uint64_t u, flexEndpoint where) {
    flexPush_(flexInsertUnsigned, u);
}

void flexPushFloat16(flex **const ff, const float value, flexEndpoint where) {
    flexPush_(flexInsertFloat16, value);
}

void flexPushFloatB16(flex **const ff, const float value, flexEndpoint where) {
    flexPush_(flexInsertFloatB16, value);
}

void flexPushFloat(flex **const ff, const float value, flexEndpoint where) {
    flexPush_(flexInsertFloat, value);
}

void flexPushDouble(flex **const ff, const double value, flexEndpoint where) {
    flexPush_(flexInsertDouble, value);
}

void flexPushBytes(flex **const ff, const void *data, size_t len,
                   flexEndpoint where) {
    flexPush_(flexInsertBytes, data, len);
}

/* Push a databox to the head/tail of `*ff`.
 *
 * The BYTES cases route through flexInsertBytes, which stores a numeral as
 * its canonical number ("22" -> integer 22, databoxCanonical).
 * flexInsertByType stores BYTES verbatim, so the two are not interchangeable
 * for byte strings and stay separate switches.  Callers that insert at a
 * position canonicalize first when they need the canonical form.  Numeric and
 * fixed-width types have no such difference, so the wide-integer and nested
 * container cases use the shared insert path instead of duplicating its
 * encoders. */
void flexPushByType(flex **const ff, const databox *box, flexEndpoint where) {
    /* Special case: if databox is NULL, insert NULL encoding. */
    if (!box) {
        flexPush_(flexInsertNull, NULL);
        return;
    }

    switch (box->type) {
    case DATABOX_BYTES:
        flexPush_(flexInsertBytes, box->data.bytes.start, databoxLen(box));
        break;
    case DATABOX_BYTES_EMBED:
        flexPush_(flexInsertBytes, box->data.bytes.embed, databoxLen(box));
        break;
    case DATABOX_SIGNED_64:
        flexPush_(flexInsertSigned, box->data.i64);
        break;
    case DATABOX_UNSIGNED_64:
    case DATABOX_PTR:
        flexPush_(flexInsertUnsigned, box->data.u64);
        break;
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
    case DATABOX_CONTAINER_FLEX_MAP:
    case DATABOX_CONTAINER_FLEX_LIST:
    case DATABOX_CONTAINER_FLEX_SET:
    case DATABOX_CONTAINER_FLEX_TUPLE:
    case DATABOX_CONTAINER_REFERENCE_EXTERNAL:
#if FLEX_ENABLE_PTR_MDSC
    case DATABOX_PTR_MDSC:
#endif
        /* No flexPush_ helper exists for these, and none of them needs
         * bytes-canonicalisation — route through the shared insert path so
         * push accepts every type flexInsertByType encodes, which includes
         * every type flexGetByType reports. */
        flexInsertByType(ff,
                         (where == FLEX_ENDPOINT_HEAD)
                             ? FLEX_ENTRY_HEAD(*ff)
                             : FLEX_ENTRY_AFTER_TAIL(*ff),
                         box);
        break;
    case DATABOX_FLOAT_32:
        flexPush_(flexInsertFloat, box->data.f32);
        break;
    case DATABOX_DOUBLE_64:
        flexPush_(flexInsertDouble, box->data.d64);
        break;
    case DATABOX_TRUE:
        flexPush_(flexInsertTrue, NULL);
        break;
    case DATABOX_FALSE:
        flexPush_(flexInsertFalse, NULL);
        break;
    case DATABOX_NULL:
        flexPush_(flexInsertNull, NULL);
        break;
    default:
        assert(NULL && "box type has no flexPushByType encoding");
        __builtin_unreachable();
    }
}

/* Exact push: store `box` as given.  Only the same-type minimal width
 * applies (integer width, lossless double-to-float); BYTES stay BYTES.
 * flexInsertByType is the raw insert the sorted and positional paths use. */
void flexPushByTypeExact(flex **const ff, const databox *box,
                         flexEndpoint where) {
    if (!box) {
        flexPush_(flexInsertNull, NULL);
        return;
    }

    flexInsertByType(ff,
                     (where == FLEX_ENDPOINT_HEAD) ? FLEX_ENTRY_HEAD(*ff)
                                                   : FLEX_ENTRY_AFTER_TAIL(*ff),
                     box);
}

void flexPushBytesExact(flex **const ff, const void *data, size_t len,
                        flexEndpoint where) {
    const databox box = databoxNewBytes(data, len);
    flexPushByTypeExact(ff, &box, where);
}

/* flex <-> databox single-element isomorphism (see flex.h). */
flex *flexNewFromDatabox(const databox *box) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f) {
        return NULL;
    }
    flexPushByType(&f, box, FLEX_ENDPOINT_TAIL);
    return f;
}

/* Build a flex from n values, given as an array of boxes or of box
 * pointers, with the row append of the chosen conversion: one
 * reallocation per chunk of values instead of one per value. */
static flex *flexNewFromBoxes_(const databox *boxes, const databox *const *ptrs,
                               const size_t n, const bool exact) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f) {
        return NULL;
    }

    enum { FACTORY_CHUNK_ = 64 };
    const databox *chunk[FACTORY_CHUNK_];
    for (size_t at = 0; at < n; at += FACTORY_CHUNK_) {
        const size_t len = n - at < FACTORY_CHUNK_ ? n - at : FACTORY_CHUNK_;
        const databox *const *row = ptrs ? ptrs + at : chunk;
        if (!ptrs) {
            for (size_t i = 0; i < len; i++) {
                chunk[i] = &boxes[at + i];
            }
        }

        if (exact) {
            flexAppendMultipleExact(&f, (uint_fast32_t)len, row);
        } else {
            flexAppendMultiple(&f, (uint_fast32_t)len, row);
        }
    }

    return f;
}

flex *flexNewFromDataboxes(const databox *boxes, size_t n) {
    return flexNewFromBoxes_(boxes, NULL, n, false);
}

flex *flexNewFromDataboxPtrs(const databox *const *boxes, size_t n) {
    return flexNewFromBoxes_(NULL, boxes, n, false);
}

flex *flexNewFromDataboxExact(const databox *box) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f) {
        return NULL;
    }
    flexPushByTypeExact(&f, box, FLEX_ENDPOINT_TAIL);
    return f;
}

flex *flexNewFromDataboxesExact(const databox *boxes, size_t n) {
    return flexNewFromBoxes_(boxes, NULL, n, true);
}

flex *flexNewFromDataboxPtrsExact(const databox *const *boxes, size_t n) {
    return flexNewFromBoxes_(NULL, boxes, n, true);
}

bool flexGetSingleDatabox(const uint8_t *buf, size_t buflen, databox *out) {
    if (!buf || !out) {
        return false;
    }
    /* Strict (deep) validation: these bytes arrive from replication / on-disk
     * recovery, so refuse a lying header rather than walk past it into the
     * entry decode below. */
    if (!flexValidateBufferDeep(buf, buflen)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    if (flexCount(f) != 1) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    if (!fe) {
        return false;
    }
    /* A plain databox cannot hold a 96/128-bit value; that needs
     * flexGetSingleDataboxBig. */
    if (flexEntryIsIntegerWide_(fe)) {
        return false;
    }
    flexGetByType(fe, out);
    return true;
}

void flexPushDataboxBig(flex **const ff, const databoxBig *b,
                        flexEndpoint where) {
    if (!b) {
        return;
    }
    switch (b->type) {
    case DATABOX_SIGNED_128: {
        /* Reconstruct a fully-formed big databox (sets ->big + ->extra) so the
         * push never trips flexInsertByType_'s assert(box->big), regardless of
         * how the caller's databoxBig was assembled. */
        databoxWidePayloadRequire((const databox *)b);
        databoxBig t;
        DATABOX_BIG_SIGNED_128(&t, *b->data.i128);
        flexPushByType(ff, (const databox *)&t, where);
        break;
    }
    case DATABOX_UNSIGNED_128: {
        databoxWidePayloadRequire((const databox *)b);
        databoxBig t;
        DATABOX_BIG_UNSIGNED_128(&t, *b->data.u128);
        flexPushByType(ff, (const databox *)&t, where);
        break;
    }
    default:
        /* 64-bit / fixed-width: the leading 16 bytes are a valid databox. */
        flexPushByType(ff, (const databox *)b, where);
        break;
    }
}

flex *flexNewFromDataboxBig(const databoxBig *b) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f) {
        return NULL;
    }
    flexPushDataboxBig(&f, b, FLEX_ENDPOINT_TAIL);
    return f;
}

void flexPushDataboxBigExact(flex **const ff, const databoxBig *b,
                             flexEndpoint where) {
    if (!b) {
        return;
    }

    switch (b->type) {
    case DATABOX_SIGNED_128: {
        databoxWidePayloadRequire((const databox *)b);
        databoxBig t;
        DATABOX_BIG_SIGNED_128(&t, *b->data.i128);
        flexPushByTypeExact(ff, (const databox *)&t, where);
        break;
    }
    case DATABOX_UNSIGNED_128: {
        databoxWidePayloadRequire((const databox *)b);
        databoxBig t;
        DATABOX_BIG_UNSIGNED_128(&t, *b->data.u128);
        flexPushByTypeExact(ff, (const databox *)&t, where);
        break;
    }
    default:
        flexPushByTypeExact(ff, (const databox *)b, where);
        break;
    }
}

flex *flexNewFromDataboxBigExact(const databoxBig *b) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (!f) {
        return NULL;
    }
    flexPushDataboxBigExact(&f, b, FLEX_ENDPOINT_TAIL);
    return f;
}

bool flexGetSingleDataboxBig(const uint8_t *buf, size_t buflen,
                             databoxBig *out) {
    if (!buf || !out) {
        return false;
    }
    if (!flexValidateBufferDeep(buf, buflen)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    if (flexCount(f) != 1) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    if (!fe) {
        return false;
    }
    flexGetByTypeBig(fe, out);
    return true;
}

bool flexGetTwoDataboxes(const uint8_t *buf, size_t buflen, databox *a,
                         databox *b) {
    if (!buf || !a || !b) {
        return false;
    }
    if (!flexValidateBufferDeep(buf, buflen)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    if (flexCount(f) != 2) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    if (!fe) {
        return false;
    }
    flexEntry *const second = flexNext(f, fe);
    if (!second) {
        return false;
    }
    /* Plain databoxes cannot hold a 96/128-bit value.  Both entries are
     * checked before either box is written, so a refusal leaves both
     * untouched. */
    if (flexEntryIsIntegerWide_(fe) || flexEntryIsIntegerWide_(second)) {
        return false;
    }
    flexGetByType(fe, a);
    flexGetByType(second, b);
    return true;
}

bool flexGetTwoDataboxesBig(const uint8_t *buf, size_t buflen, databoxBig *a,
                            databoxBig *b) {
    if (!buf || !a || !b) {
        return false;
    }
    if (!flexValidateBufferDeep(buf, buflen)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    if (flexCount(f) != 2) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    if (!fe) {
        return false;
    }
    flexEntry *const second = flexNext(f, fe);
    if (!second) {
        return false;
    }
    flexGetByTypeBig(fe, a);
    flexGetByTypeBig(second, b);
    return true;
}

int flexFactorySelfTest(void) {
    int fails = 0;
#define FFT(cond)                                                              \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "flexFactorySelfTest FAIL: %s (%s:%d)\n", #cond,   \
                    __FILE__, __LINE__);                                       \
            fails++;                                                           \
        }                                                                      \
    } while (0)

    /* single non-numeric bytes round-trip — stays bytes. */
    {
        databox in = databoxNewBytes("hello", 5);
        flex *f = flexNewFromDatabox(&in);
        FFT(f != NULL);
        databox out = {0};
        FFT(flexGetSingleDatabox((const uint8_t *)f, flexBytes(f), &out));
        FFT(DATABOX_IS_BYTES(&out) && databoxLen(&out) == 5 &&
            memcmp(databoxBytes(&out), "hello", 5) == 0);
        flexFree(f);
    }

    /* numeric byte-string canonicalises to an integer (push semantics). */
    {
        databox in = databoxNewBytes("22", 2);
        flex *f = flexNewFromDatabox(&in);
        databox out = {0};
        FFT(flexGetSingleDatabox((const uint8_t *)f, flexBytes(f), &out));
        FFT((out.type == DATABOX_UNSIGNED_64 && out.data.u == 22) ||
            (out.type == DATABOX_SIGNED_64 && out.data.i == 22));
        flexFree(f);
    }

    /* signed integer round-trip. */
    {
        databox in = databoxNewSigned(-7);
        flex *f = flexNewFromDatabox(&in);
        databox out = {0};
        FFT(flexGetSingleDatabox((const uint8_t *)f, flexBytes(f), &out));
        FFT(out.data.i == -7);
        flexFree(f);
    }

    /* multi-element factory: N values, correct count + iteration order. */
    {
        databox boxes[3];
        boxes[0] = databoxNewBytes("alpha", 5);
        boxes[1] = databoxNewBytes("zz", 2);
        boxes[2] = databoxNewUnsigned(99);
        flex *f = flexNewFromDataboxes(boxes, 3);
        FFT(f && flexCount(f) == 3);
        size_t i = 0;
        flexEntry *fe = flexHead(f);
        while (fe) {
            i++;
            fe = flexNext(f, fe);
        }
        FFT(i == 3);
        flexFree(f);
    }

    /* empty factory yields a valid zero-element flex. */
    {
        flex *f = flexNewFromDataboxes(NULL, 0);
        FFT(f && flexCount(f) == 0);
        flexFree(f);
    }

    /* pointer-array factory (multimap convention): N boxes incl a non-last
     * BYTES value — the case the old hand-rolled multimap delta couldn't
     * encode.  Verify count + that the leading BYTES element round-trips. */
    {
        databox b0 = databoxNewBytes("member-key", 10);
        databox b1 = databoxNewUnsigned(7);
        databox b2 = databoxNewBytes("tail", 4);
        const databox *ptrs[3] = {&b0, &b1, &b2};
        flex *f = flexNewFromDataboxPtrs(ptrs, 3);
        FFT(f && flexCount(f) == 3);
        databox out0 = {0};
        flexEntry *fe = flexHead(f);
        flexGetByType(fe, &out0);
        FFT(DATABOX_IS_BYTES(&out0) && databoxLen(&out0) == 10 &&
            memcmp(databoxBytes(&out0), "member-key", 10) == 0);
        flexFree(f);
    }

    /* defensive unwrap: a multi-element flex is rejected by the single-getter.
     */
    {
        databox boxes[2] = {databoxNewBytes("x", 1), databoxNewBytes("y", 1)};
        flex *f = flexNewFromDataboxes(boxes, 2);
        databox out = {0};
        FFT(!flexGetSingleDatabox((const uint8_t *)f, flexBytes(f), &out));
        flexFree(f);
    }

    /* defensive unwrap: a garbage buffer is rejected, not walked. */
    {
        uint8_t garbage[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        databox out = {0};
        FFT(!flexGetSingleDatabox(garbage, sizeof(garbage), &out));
    }

    /* two-element unwrap ([index,value] shape) + arity rejection. */
    {
        databox idx = databoxNewUnsigned(3);
        databox val = databoxNewBytes("payload", 7);
        const databox *ptrs[2] = {&idx, &val};
        flex *f = flexNewFromDataboxPtrs(ptrs, 2);
        databox a = {0}, b = {0};
        FFT(flexGetTwoDataboxes((const uint8_t *)f, flexBytes(f), &a, &b));
        FFT(a.data.u == 3 && DATABOX_IS_BYTES(&b) && databoxLen(&b) == 7 &&
            memcmp(databoxBytes(&b), "payload", 7) == 0);
        flexFree(f);
        /* a 1-element flex must be rejected by the two-getter. */
        flex *f1 = flexNewFromDatabox(&idx);
        FFT(!flexGetTwoDataboxes((const uint8_t *)f1, flexBytes(f1), &a, &b));
        flexFree(f1);
    }

    /* databoxBig arm — 64-bit round-trip. */
    {
        databoxBig in;
        DATABOX_BIG_INIT(&in);
        in.type = DATABOX_UNSIGNED_64;
        in.data.u64 = 1234567890123ULL;
        flex *f = flexNewFromDataboxBig(&in);
        databoxBig out;
        FFT(flexGetSingleDataboxBig((const uint8_t *)f, flexBytes(f), &out));
        FFT(out.data.u == 1234567890123ULL);
        flexFree(f);
    }

    /* databoxBig arm — NEGATIVE signed 128-bit round-trip (|value| > 2^64;
     * stays SIGNED_128).  Exercises flexPrepareSignedBig — an int64_t return
     * there silently truncated the high 64 bits. */
    {
        const __int128_t big = -(((__int128_t)1 << 100) + 7);
        databoxBig in;
        DATABOX_BIG_SIGNED_128(&in, big);
        flex *f = flexNewFromDataboxBig(&in);
        databoxBig out;
        FFT(flexGetSingleDataboxBig((const uint8_t *)f, flexBytes(f), &out));
        FFT(out.type == DATABOX_SIGNED_128 && *out.data.i128 == big);
        flexFree(f);
    }

    /* A POSITIVE 128-bit value carried as SIGNED canonicalises to UNSIGNED_128
     * (flex stores all positive integers unsigned) — value-preserving, which
     * is all that matters for a numeric set. */
    {
        const __int128_t big = ((__int128_t)1 << 100) + 7;
        databoxBig in;
        DATABOX_BIG_SIGNED_128(&in, big);
        flex *f = flexNewFromDataboxBig(&in);
        databoxBig out;
        FFT(flexGetSingleDataboxBig((const uint8_t *)f, flexBytes(f), &out));
        FFT(out.type == DATABOX_UNSIGNED_128 &&
            (__int128_t)*out.data.u128 == big);
        flexFree(f);
    }

    /* databoxBig arm — unsigned 128-bit round-trip. */
    {
        const __uint128_t big = ((__uint128_t)1 << 120) + 99;
        databoxBig in;
        DATABOX_BIG_UNSIGNED_128(&in, big);
        flex *f = flexNewFromDataboxBig(&in);
        databoxBig out;
        FFT(flexGetSingleDataboxBig((const uint8_t *)f, flexBytes(f), &out));
        FFT(out.type == DATABOX_UNSIGNED_128 && *out.data.u128 == big);
        flexFree(f);
    }

    if (fails == 0) {
        fprintf(stderr, "flexFactorySelfTest: OK\n");
    }
#undef FFT
    return fails;
}

/* ====================================================================
 * flex index retrieval
 * ==================================================================== */
/* Returns pointer to element at position 'index' (if exists). */
flexEntry *flexIndexDirect(const flex *f, int32_t index) {
    const flexEntry *head = FLEX_ENTRY_HEAD(f);
    const flexEntry *end = GET_END(f);

    flexEntry *fe;
    if (index < 0) {
        /* Reverse indices are 1 based, not zero based.
         * (i.e. when going in reverse, the "first last element" is -1
         *  and not zero like when going forward.  So, when going backwards,
         *  our indices go from -1 to -COUNT instead of from 0 to [COUNT -
         * 1] */
        index = (-index) - 1;
        fe = flexTail(f);
        if (fe != head) {
            /* Get size of encoding for entry walking backwards */
            varintWidth prevlensize;
            size_t prevlen;
            FLEX_DECODE_LENGTH_REVERSE(fe - 1, prevlensize, prevlen);
            /* For 'index' steps, walk backwards through the list. */
            while (index--) {
                fe -= (prevlen +
                       FLEX_ENCODING_SIZE_TOTAL_REVERSE(fe - 1, prevlensize));
                FLEX_DECODE_LENGTH_REVERSE(fe - 1, prevlensize, prevlen);
                if (fe == head) {
                    break;
                }
            }
        }
    } else {
        fe = (flexEntry *)head;
        while (index--) {
            fe += flexRawEntryLength(fe);
            if (fe == end) {
                break;
            }
        }
    }

    return (fe == end || index > 0) ? NULL : fe;
}

/* Pre-process index request.
 * If user asks for an element more than 50% through the list, rewrite
 * the traversal to be from the endpoint closest to the element. */
flexEntry *flexIndex(const flex *f, int32_t index) {
    const int_fast32_t count = flexCount_(f);
    const int_fast32_t halfCount = count / 2;

    if (index > 0) {
        if (index < count && index > halfCount) {
            /* If we're using a forward index for an element more than half
             * way through the list, convert to a reverse traversal. */
            index = -(count - index);
        }
    } else {
        if ((-index) <= count && (-index) > halfCount) {
            /* If we're using a reverse index for an element more than half
             * way through the list, convert to a forward traversal. */
            index += count;
        }
    }

    return flexIndexDirect(f, index);
}

/* 3-way anchored index retrieval: walk to element 'index' from the
 * nearest of head (forward), the caller's anchor entry (either
 * direction), or tail (backward).  'anchorElemIdx' is the anchor
 * entry's 0-based forward element index; a NULL anchor or out-of-range
 * anchor index degrades to the two-way endpoint walk.  Index
 * convention matches flexIndex(): 0-based forward, negative = 1-based
 * from the tail; returns NULL when out of range. */
flexEntry *flexIndexFromAnchor(const flex *f, int32_t index,
                               const flexEntry *anchorFE,
                               int32_t anchorElemIdx) {
    const int_fast32_t count = flexCount_(f);

    /* Normalize to a forward element index (negative is 1-based) */
    const int_fast32_t fwdIdx = index < 0 ? count + index : index;
    if (fwdIdx < 0 || fwdIdx >= count) {
        return NULL;
    }

    if (anchorFE && anchorElemIdx >= 0 && anchorElemIdx < count) {
        const int_fast32_t dHead = fwdIdx;
        const int_fast32_t dTail = (count - 1) - fwdIdx;
        const int_fast32_t dAnchor = fwdIdx > anchorElemIdx
                                         ? fwdIdx - anchorElemIdx
                                         : anchorElemIdx - fwdIdx;

        if (dAnchor < dHead && dAnchor < dTail) {
            /* The anchor is strictly nearest: walk out from it.  Steps
             * stay inside [0, count - 1], so Next/Prev never run off
             * either end. */
            flexEntry *fe = (flexEntry *)anchorFE;
            if (fwdIdx >= anchorElemIdx) {
                for (int_fast32_t i = dAnchor; i; i--) {
                    fe = flexNext(f, fe);
                }
            } else {
                for (int_fast32_t i = dAnchor; i; i--) {
                    fe = flexPrev(f, fe);
                }
            }

            return fe;
        }
    }

    /* head or tail is nearest: existing two-way endpoint logic */
    return flexIndex(f, index);
}

/* Anchored index specialized to a MAINTAINED sorted-map middle (the
 * entry flexMiddle() returns; multimap tiers cache one), whose element
 * index is ((count / elementsPerEntry) / 2) * elementsPerEntry by
 * construction. */
flexEntry *flexIndexWithMiddle(const flex *f,
                               const uint_fast32_t elementsPerEntry,
                               int32_t index, const flexEntry *middle) {
    const int_fast32_t count = flexCount_(f);
    const int32_t middleIdx =
        (int32_t)(((count / elementsPerEntry) / 2) * elementsPerEntry);
    return flexIndexFromAnchor(f, index, middle, middleIdx);
}

bool flexEntryIsValid(const flex *f, flexEntry *fe) {
    if (fe >= f + flexTotalBytes_(f) || fe < f) {
        return false;
    }

    return true;
}

flexEntry DK_FN_PURE *flexHead(const flex *f) {
    return FLEX_ENTRY_HEAD(f);
}

flexEntry DK_FN_PURE *flexHeadIfAny(const flex *f) {
    flexEntry *const head = FLEX_ENTRY_HEAD(f);
    return head == GET_END(f) ? NULL : head;
}

flexEntry *flexMiddle(const flex *f, const uint_fast32_t elementsPerEntry) {
    const size_t count = flexCount_(f);
    return count ? flexIndexDirect(f, ((count / elementsPerEntry) / 2) *
                                          elementsPerEntry)
                 : flexHead(f);
}

/* ====================================================================
 * flex iteration prev/next
 * ==================================================================== */
flexEntry *flexNext(const flex *const f, flexEntry *fe) {
    const flexEntry *end = GET_END(f);

    /* If we're at end, don't get Next() */
    if (fe == end) {
        return NULL;
    }

    fe += flexRawEntryLength(fe);

    /* If we got Next and Next is 'end', don't get Next() */
    if (fe == end) {
        return NULL;
    }

    return fe;
}

flexEntry *flexPrev(const flex *const f, flexEntry *fe) {
    if (IS_HEAD(fe, f)) {
        return NULL;
    }

    return flexGetPreviousEntry(fe);
}

/* ====================================================================
 * flex element retrieval
 * ==================================================================== */
/* The one decoder.  Every flexGetByType* entry point is this with its
 * own choice of wide storage, and narrow values decode identically
 * through all of them.  Returns the entry's encoding so the reference
 * variants can tell a reference id from a stored value. */
DK_INLINE_ALWAYS flexEncoding flexDecode_(const flexEntry *const fe,
                                          databox *const box,
                                          databoxWide *const slot,
                                          const flexWideStore_ store) {
    flexEntryData entry = {0};
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        box->data.bytes.start = FLEX_ENTRY_DATA(&entry);
        box->len = entry.len;
        box->type = DATABOX_BYTES;
    } else {
        flexLoadFixedLength_(&entry, box, slot, store);
    }

    return entry.encoding;
}

/* Replace the reference id decoded into 'box' with the value it names
 * in 'referenceContainer'.  A 96/128-bit value goes to 'slot' as in
 * flexGetByTypeInto (deferred with a NULL slot).  Any other value
 * writes only type, data and len, like a narrow decode.  An id the
 * container doesn't hold stays as the id. */
static void flexResolveReferenceInto_(databox *const box,
                                      databoxWide *const slot,
                                      const multimapAtom *referenceContainer) {
    /* multimapAtomLookupConvertBig reads only the id's data and type. */
    databoxBig resolved = {.data = box->data, .type = box->type};
    multimapAtomLookupConvertBig(referenceContainer, &resolved);

    box->type = resolved.type;
    if (resolved.big) {
        if (slot) {
            memcpy(slot, resolved.extra, sizeof(*slot));
        }

        box->data.ptr = slot;
        box->big = slot != NULL;
        box->allocated = false;
    } else {
        box->data = resolved.data;
        box->len = resolved.len;
    }
}

void flexGetByTypeInto(const flexEntry *const fe, databox *box,
                       databoxWide *slot) {
    flexDecode_(fe, box, slot, FLEX_WIDE_STORE_SLOT_);
}

void flexGetByTypeIntoWithReference(const flexEntry *const fe, databox *box,
                                    databoxWide *slot,
                                    const multimapAtom *referenceContainer) {
    if (FLEX_IS_REF_EXTERNAL(
            flexDecode_(fe, box, slot, FLEX_WIDE_STORE_SLOT_))) {
        flexResolveReferenceInto_(box, slot, referenceContainer);
    }
}

void flexGetByType(const flexEntry *const fe, databox *box) {
    flexDecode_(fe, box, NULL, FLEX_WIDE_STORE_IN_BAND_);
}

void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const multimapAtom *referenceContainer) {
    if (FLEX_IS_REF_EXTERNAL(
            flexDecode_(fe, box, NULL, FLEX_WIDE_STORE_IN_BAND_))) {
        /* TODO: make copying version here if atom map is compressed? */
        multimapAtomLookupConvert(referenceContainer, box);
    }
}

void flexGetByTypeCopy(const flexEntry *const fe, databox *box) {
    /* get at fe, populate type in box, populate box, return found */
    flexEntryData entry = {0};
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        /* @own: caller; free=databoxFreeData */
        box->data.bytes.start = zcalloc(1, entry.len);
        memcpy(box->data.bytes.start, FLEX_ENTRY_DATA(&entry), entry.len);
        box->len = entry.len;
        box->type = DATABOX_BYTES;
        box->allocated = true;
        box->big = false;
    } else {
        /* A 96/128-bit value goes where 'box' already points when big is
         * set, with its owner unchanged, and otherwise to a heap block the
         * box owns, so the result never depends on storage outside the
         * box.  flexWideTarget_ sets up both. */
        flexLoadFixedLength_(&entry, box, NULL, FLEX_WIDE_STORE_OWNED_);
        if (box->type == DATABOX_SIGNED_128 ||
            box->type == DATABOX_UNSIGNED_128) {
            return;
        }

        /* Nothing here borrows a wide pointer, and only a copied
         * subcontainer below owns memory. */
        box->big = false;
        box->allocated = false;
        if (FLEX_IS_FORWARD_DECLARE_SUBCONTAINER(entry.encoding)) {
            /* copy the flex or cflex at 'box->data' to
             * newly allocated memory then attach it to the box. */
            /* @own: caller; free=databoxFreeData */
            void *tmp = zcalloc(1, box->len);
            memcpy(tmp, databoxBytes(box), databoxLen(box));
            box->data.bytes.start = tmp;
            box->allocated = true;
        }
    }
}

bool flexGetNextByType(flex *const f, flexEntry **const fe, databox *box) {
    *fe = flexNext(f, *fe);

    if (!*fe) {
        return false;
    }

    flexGetByType(*fe, box);
    return true;
}

void flexGetByTypeBig(const flexEntry *const fe, databoxBig *box) {
    /* Clear every header bit so the result never depends on the box's
     * prior state, then decode with 'extra' as the slot. */
    *(databox *)box = (databox){{0}};
    flexDecode_(fe, (databox *)box, (databoxWide *)box->extra,
                FLEX_WIDE_STORE_SLOT_);
}

bool flexGetNextByTypeBig(flex *const f, flexEntry **const fe,
                          databoxBig *box) {
    *fe = flexNext(f, *fe);

    if (!*fe) {
        return false;
    }

    flexGetByTypeBig(*fe, box);
    return true;
}

/* flexGetSigned and flexGetUnsigned only need to know that a value is
 * wide, never its payload, so they decode with no slot: a wide value
 * comes back deferred and nothing is written for it. */
bool flexGetSigned(flexEntry *fe, int64_t *value) {
    databox box;
    flexGetByTypeInto(fe, &box, NULL);

    if (box.type == DATABOX_SIGNED_128 || box.type == DATABOX_UNSIGNED_128) {
        /* Stored wide means the value does not fit 64 bits. */
        return false;
    }

    if (box.type == DATABOX_UNSIGNED_64 && box.data.u64 > INT64_MAX) {
        /* If our integer is larger than our return type,
         * we can't return a proper value.  Fail. */
        return false;
    }

    *value = box.data.i64;
    return true;
}

bool flexGetUnsigned(flexEntry *fe, uint64_t *value) {
    databox box;
    flexGetByTypeInto(fe, &box, NULL);

    if (box.type == DATABOX_SIGNED_128 || box.type == DATABOX_UNSIGNED_128) {
        /* Stored wide means the value does not fit 64 bits. */
        return false;
    }

    if (box.type == DATABOX_SIGNED_64 && box.data.i64 < 0) {
        /* we can't return negative integers! */
        return false;
    }

    *value = box.data.u64;
    return true;
}

/* ====================================================================
 * flex delete operations
 * ==================================================================== */
/* Delete 'count' entries at '*fe' and leave '*fe' at the entry that
 * followed them (or at the end).  A delete can shrink the header, which
 * moves every entry down by the header's change, so the cursor is
 * restored from its offset plus that change, not from the offset alone. */
DK_INLINE_ALWAYS void flexDeleteKeepCursor_(flex **const ff, flexEntry **fe,
                                            const int_fast32_t count,
                                            const bool drain) {
    assert(ff && *ff && fe && *fe);

    const size_t offset = *fe - *ff;
    flexHeaderInfo headerInfo = {0};
    flexDelete___(ff, *fe, count, drain, &headerInfo, NULL);
    *fe = *ff + offset + headerInfo.headerDiff;
}

/* Delete entry at 'fe' while maintaining the validity of 'fe' after the
 * deletion (and potential shrink-induced realloc) */
void flexDelete(flex **const ff, flexEntry **fe) {
    flexDeleteKeepCursor_(ff, fe, 1, false);
}

/* Same as flexDelete() but don't update fe */
void flexDeleteNoUpdateEntry(flex **const ff, flexEntry *fe) {
    assert(fe);

    flexDelete__(ff, fe, 1, false);
}

void flexDeleteDrain(flex **const ff, flexEntry **fe) {
    flexDeleteKeepCursor_(ff, fe, 1, true);
}

/* Delete 'num' entries from flex starting at 'fe'.
 * Also update *fe in place, to be able to iterate over the
 * flex, while deleting entries. */
void flexDeleteCount(flex **const ff, flexEntry **fe, const uint32_t count) {
    flexDeleteKeepCursor_(ff, fe, count, false);
}

void flexDeleteSortedValueWithMiddle(flex **const ff,
                                     const uint_fast32_t elementsPerEntry,
                                     flexEntry *const fe,
                                     flexEntry **middleEntry) {
    assert(ff && *ff && fe && middleEntry && *middleEntry);

    const size_t initialCount = flexCount_(*ff) / elementsPerEntry;
    const bool atEvenToOddTransitionBoundary = initialCount % 2 == 0;
    const bool deletingBeforeMiddle = fe < *middleEntry;

    if (atEvenToOddTransitionBoundary && !deletingBeforeMiddle) {
        int_fast32_t dropElements = elementsPerEntry;
        while (dropElements--) {
            *middleEntry = flexGetPreviousEntry(*middleEntry);
        }
    } else if (!atEvenToOddTransitionBoundary && deletingBeforeMiddle) {
        int_fast32_t dropElements = elementsPerEntry;
        while (dropElements--) {
            *middleEntry += flexRawEntryLength(*middleEntry);
        }
    }

    flexHeaderInfo headerInfo = {0};
    const size_t offsetMiddle = *middleEntry - *ff;
    flexDelete___(ff, fe, elementsPerEntry, false, &headerInfo, NULL);
    *middleEntry = *ff + offsetMiddle + headerInfo.headerDiff;

    if (deletingBeforeMiddle) {
        *middleEntry += headerInfo.insertedBytes;
    }

#ifdef FLEX_DEBUG_EXTENSIVE
    assert(*middleEntry == flexMiddle(*ff, elementsPerEntry));
#endif
}

void flexDeleteCountDrain(flex **const ff, flexEntry **fe,
                          const uint32_t count) {
    flexDeleteKeepCursor_(ff, fe, count, true);
}

void flexDeleteOffsetCount(flex **const ff, const int32_t offset,
                           const uint32_t count) {
    flexEntry *fe;
    FLEX_INDEX(*ff, offset, fe);
    flexDeleteCount(ff, &fe, count);
}

void flexDeleteOffsetCountDrain(flex **const ff, const int32_t offset,
                                const uint32_t count) {
    flexEntry *fe;
    FLEX_INDEX(*ff, offset, fe);
    flexDeleteCountDrain(ff, &fe, count);
}

/* Delete a range of entries from the flex. */
void flexDeleteRange(flex **const ff, const int32_t index,
                     const uint32_t count) {
    flexEntry *fe;
    FLEX_INDEX(*ff, index, fe);
    if (fe) {
        flexDelete__(ff, fe, count, false);
    }
}

void flexDeleteUpToInclusive(flex **ff, flexEntry *fe) {
    /* Need to:
     *   - discover the index of 'fe' in '*ff'
     *   - delete from [head, index]. */

    /* If we got here without a valid entry, don't crash, just
     * give up because we can't delete anything reasonable. */
    if (!fe) {
        return;
    }

    flexEntry *head = flexHead(*ff);
    assert(fe >= head);

    if (fe > flexTail(*ff)) {
        /* delete position is after tail, so remove everything. */
        flexReset(ff);
        return;
    }

    if (fe == head) {
        /* nothing to delete */
        return;
    }

    flexEntry *current = fe;

    /* '1' because if 'fe' == 'head', then we don't enter the while
     * loop and the first entry would never be counted */
    int_fast32_t discoveredCount = 1;
    while (current != head) {
        /* count elements from current back to head */
        current = flexGetPreviousEntry(current);
        discoveredCount++;
    }

    flexDeleteCount(ff, &head, discoveredCount);
}

void flexDeleteUpToInclusivePlusN(flex **ff, flexEntry *fe,
                                  const int32_t nMore) {
    flexEntry *const tail = flexTail(*ff);
    for (int_fast32_t i = 0; i < nMore; i++) {
        if (fe > tail) {
            fe = tail;
            break;
        }

        fe = flexNext(*ff, fe);
    }

    flexDeleteUpToInclusive(ff, fe);
}

flex *flexSplitRange(flex **const ff, const int32_t index, const uint32_t num) {
    flexEntry *fe;
    FLEX_INDEX(*ff, index, fe);

    /* @own: caller; free=flexFree */
    flex *removed = flexNew();
    if (fe) {
        flexDelete__GetDeleted(ff, fe, num, false, &removed);
    }

    return removed;
}

/* Delete a range of entries from the flex. */
void flexDeleteRangeDrain(flex **const ff, const int32_t index,
                          const uint32_t num) {
    flexEntry *fe;
    FLEX_INDEX(*ff, index, fe);
    if (fe) {
        flexDelete__(ff, fe, num, true);
    }
}

/* ====================================================================
 * flex compare
 * ==================================================================== */
/* Compare entry pointer to by 'fe' with 'data' of length 'len'. */
/* This is unused except for tests */
#ifdef DATAKIT_TEST
bool flexCompareBytes(flexEntry *fe, const void *data, const size_t len) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        /* Raw compare */
        if (entry.len == len) {
            return (memcmp(fe + entry.encodingSize, data, len) == 0);
        }

        return false;
    }

    if (flexEntryIsIntegerNarrow_(fe)) {
        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        int64_t val;
        if (len <= 32 && StrBufToInt64(data, len, &val)) {
            int64_t zval = flexLoadSigned(&entry);
            return zval == val;
        }
    }
    return false;
}
#endif

/* The 64-bit integer at 'entry' as a value: true with '*neg' and '*mag'
 * (the magnitude) set, false for any other entry. */
DK_STATIC bool flexEntryInteger_(const flexEntryData *entry, bool *neg,
                                 uint64_t *mag) {
    if (!flexEntryIsIntegerNarrow_(entry->fe)) {
        return false;
    }

    databox box = {{0}};
    flexGetByTypeInto(entry->fe, &box, NULL);
    if (box.type == DATABOX_SIGNED_64) {
        *neg = box.data.i64 < 0;
        *mag = *neg ? (uint64_t)0 - (uint64_t)box.data.i64
                    : (uint64_t)box.data.i64;
        return true;
    }

    if (box.type == DATABOX_UNSIGNED_64) {
        *neg = false;
        *mag = box.data.u64;
        return true;
    }

    return false;
}

/* Integer finds match by mathematical value: -1 never matches a stored
 * UINT64_MAX, and 5 matches 5 however it is encoded. */
DK_STATIC bool flexEntryCompareSigned__(const flexEntryData *entry,
                                        const uint8_t *src, const size_t len) {
    DK_NOTUSED(len);
    int64_t want;
    memcpy(&want, src, sizeof(want));
    bool neg;
    uint64_t mag;
    if (!flexEntryInteger_(entry, &neg, &mag)) {
        return false;
    }

    const bool wantNeg = want < 0;
    const uint64_t wantMag =
        wantNeg ? (uint64_t)0 - (uint64_t)want : (uint64_t)want;
    return neg == wantNeg && mag == wantMag;
}

DK_STATIC bool flexEntryCompareUnsigned__(const flexEntryData *entry,
                                          const uint8_t *src,
                                          const size_t len) {
    DK_NOTUSED(len);
    uint64_t want;
    memcpy(&want, src, sizeof(want));
    bool neg;
    uint64_t mag;
    return flexEntryInteger_(entry, &neg, &mag) && !neg && mag == want;
}

DK_STATIC bool flexEntryCompareString__(const flexEntryData *entry,
                                        const uint8_t *src, const size_t len) {
    /* The empty string has an encoding of its own. */
    if (entry->encoding == FLEX_BYTES_EMPTY) {
        return len == 0;
    }

    if (FLEX_IS_STR(entry->encoding) && (entry->len == len)) {
        return len == 0 || memcmp(FLEX_ENTRY_DATA(entry), src, len) == 0;
    }

    return false;
}

bool flexCompareString(flexEntry *fe, const void *str, size_t len) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    return flexEntryCompareString__(&entry, str, len);
}

bool flexCompareUnsigned(flexEntry *fe, uint64_t val) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    return flexEntryCompareUnsigned__(&entry, (uint8_t *)&val, sizeof(val));
}

bool flexCompareSigned(flexEntry *fe, int64_t val) {
    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    return flexEntryCompareSigned__(&entry, (uint8_t *)&val, sizeof(val));
}

/* ====================================================================
 * flex container math (the numeric policy: exact integers, refusal)
 * ==================================================================== */
typedef enum flexMathOp_ {
    FLEX_MATH_ADD_,
    FLEX_MATH_SUBTRACT_,
    FLEX_MATH_MULTIPLY_,
} flexMathOp_;

/* The integer entry 'fe' as a 128-bit value (any width); false for an
 * entry that is not an integer or an unsigned value above INT128_MAX */
DK_STATIC bool flexMathInteger_(const flexEntry *fe, __int128_t *out) {
    databox box;
    databoxWide wide;
    flexGetByTypeInto(fe, &box, &wide);
    switch (box.type) {
    case DATABOX_SIGNED_64:
        *out = box.data.i64;
        return true;
    case DATABOX_UNSIGNED_64:
        *out = box.data.u64;
        return true;
    case DATABOX_SIGNED_128:
        *out = *box.data.i128;
        return true;
    case DATABOX_UNSIGNED_128:
        if (*box.data.u128 >> 127) {
            return false;
        }
        *out = (__int128_t)*box.data.u128;
        return true;
    default:
        return false;
    }
}

/* Fold the integers of 'f' with 'op' (subtract: the first minus the rest)
 * inside [lo, hi]; false for a non-integer entry, an entry or partial
 * result outside the range, leaving '*result' unset */
DK_STATIC bool flexMathIntegers_(const flex *f, flexMathOp_ op, __int128_t lo,
                                 __int128_t hi, __int128_t *result) {
    __int128_t acc = op == FLEX_MATH_MULTIPLY_ ? 1 : 0;
    bool first = true;
    flexEntry *fe = flexHead(f);
    for (size_t n = flexCount(f); n; n--, fe = flexNext(f, fe)) {
        __int128_t v;
        if (!flexMathInteger_(fe, &v) || v < lo || v > hi) {
            return false;
        }
        if (op == FLEX_MATH_SUBTRACT_ && first) {
            acc = v;
        } else if (op == FLEX_MATH_MULTIPLY_) {
            if (__builtin_mul_overflow(acc, v, &acc)) {
                return false;
            }
        } else if (__builtin_add_overflow(
                       acc, op == FLEX_MATH_SUBTRACT_ ? -v : v, &acc)) {
            return false;
        }
        if (acc < lo || acc > hi) {
            return false;
        }
        first = false;
    }
    *result = acc;
    return true;
}

/* Fold the numbers of 'f' (integers and reals alike) in double; false for
 * a non-number or an inf / NaN result */
DK_STATIC bool flexMathReals_(const flex *f, flexMathOp_ op, double *result) {
    double acc = op == FLEX_MATH_MULTIPLY_ ? 1 : 0;
    bool first = true;
    flexEntry *fe = flexHead(f);
    for (size_t n = flexCount(f); n; n--, fe = flexNext(f, fe)) {
        databox box;
        databoxWide wide;
        flexGetByTypeInto(fe, &box, &wide);
        double v;
        __int128_t whole;
        if (box.type == DATABOX_FLOAT_32) {
            v = box.data.f32;
        } else if (box.type == DATABOX_DOUBLE_64) {
            v = box.data.d64;
        } else if (flexMathInteger_(fe, &whole)) {
            v = (double)whole;
        } else if (box.type == DATABOX_UNSIGNED_128) {
            v = (double)*box.data.u128;
        } else {
            return false;
        }

        if (op == FLEX_MATH_SUBTRACT_ && first) {
            acc = v;
        } else if (op == FLEX_MATH_MULTIPLY_) {
            acc *= v;
        } else {
            acc += op == FLEX_MATH_SUBTRACT_ ? -v : v;
        }
        if (!isfinite(acc)) {
            return false;
        }
        first = false;
    }
    *result = acc;
    return true;
}

#define FLEX_MATH_SIGNED_(name, op)                                            \
    bool name(const flex *f, int64_t *result) {                                \
        __int128_t r;                                                          \
        if (!flexMathIntegers_(f, op, INT64_MIN, INT64_MAX, &r)) {             \
            return false;                                                      \
        }                                                                      \
        *result = (int64_t)r;                                                  \
        return true;                                                           \
    }

#define FLEX_MATH_UNSIGNED_(name, op)                                          \
    bool name(const flex *f, uint64_t *result) {                               \
        __int128_t r;                                                          \
        if (!flexMathIntegers_(f, op, 0, UINT64_MAX, &r)) {                    \
            return false;                                                      \
        }                                                                      \
        *result = (uint64_t)r;                                                 \
        return true;                                                           \
    }

#define FLEX_MATH_DOUBLE_(name, op)                                            \
    bool name(const flex *f, double *result) {                                 \
        return flexMathReals_(f, op, result);                                  \
    }

/* The double fold narrowed to float: refused when it does not stay
 * finite in float */
#define FLEX_MATH_FLOAT_(name, op)                                             \
    bool name(const flex *f, float *result) {                                  \
        double r;                                                              \
        if (!flexMathReals_(f, op, &r) || !isfinite((float)r)) {               \
            return false;                                                      \
        }                                                                      \
        *result = (float)r;                                                    \
        return true;                                                           \
    }

FLEX_MATH_SIGNED_(flexAddSigned, FLEX_MATH_ADD_)
FLEX_MATH_SIGNED_(flexSubtractSigned, FLEX_MATH_SUBTRACT_)
FLEX_MATH_SIGNED_(flexMultiplySigned, FLEX_MATH_MULTIPLY_)
FLEX_MATH_UNSIGNED_(flexAddUnsigned, FLEX_MATH_ADD_)
FLEX_MATH_UNSIGNED_(flexSubtractUnsigned, FLEX_MATH_SUBTRACT_)
FLEX_MATH_UNSIGNED_(flexMultiplyUnsigned, FLEX_MATH_MULTIPLY_)
FLEX_MATH_FLOAT_(flexAddFloat, FLEX_MATH_ADD_)
FLEX_MATH_FLOAT_(flexSubtractFloat, FLEX_MATH_SUBTRACT_)
FLEX_MATH_FLOAT_(flexMultiplyFloat, FLEX_MATH_MULTIPLY_)
FLEX_MATH_DOUBLE_(flexAddDouble, FLEX_MATH_ADD_)
FLEX_MATH_DOUBLE_(flexSubtractDouble, FLEX_MATH_SUBTRACT_)
FLEX_MATH_DOUBLE_(flexMultiplyDouble, FLEX_MATH_MULTIPLY_)

/* ====================================================================
 * flex find / search
 * ==================================================================== */
DK_STATIC flexEntry *
flexFind_(const flex *f, flexEntry *fe, const uint32_t skip, const bool forward,
          const uint8_t *source, const uint32_t sourceLen,
          bool (*findCompare)(const flexEntryData *entry, const uint8_t *src,
                              const size_t len)) {
    if (!fe) {
        return NULL;
    }

    const flexEntry *end = GET_END(f);

    /* Empty flex (or a tail/end sentinel handed in): nothing to walk,
     * and decoding the phantom entry at `end` would over-read. */
    if (fe == end) {
        return NULL;
    }

    int32_t skipCount = 0;
    flexEntry *head = NULL;
    if (!forward) {
        head = FLEX_ENTRY_HEAD(f);
    }

    while (true) {
        flexEntryData entry = {0};
        flexEntryDataPopulate(fe, &entry);

        if (skipCount == 0) {
            if (findCompare(&entry, source, sourceLen)) {
                return fe;
            }

            /* Reset skip count */
            skipCount = skip;
        } else {
            /* Skip entry */
            skipCount--;
        }

        /* Move to next entry; the not-found terminating conditions
         * are embedded here. */
        if (forward) {
            fe = FLEX_ENTRY_NEXT_(fe, entry.len, entry.encodingSize);
            if (fe == end) {
                break;
            }
        } else {
            /* Terminate AFTER comparing the head entry (above), and
             * never step before it — head is the first real entry, so
             * flexGetPreviousEntry(head) would read into the header. */
            if (fe == head) {
                break;
            }

            fe = flexGetPreviousEntry(fe);
        }
    }

    return NULL;
}

#define moveByOffsets_(mid, elementsPerEntry, fe_, prevOffsetIndexPhysical)    \
    do {                                                                       \
        const ssize_t offsetIndexPhysical = (mid) * (elementsPerEntry);        \
        /* This diff is the number of *entries* to jump over,                  \
         * so a storage class of 2 billion is definitely enough. */            \
        int_fast32_t diff = offsetIndexPhysical - (prevOffsetIndexPhysical);   \
                                                                               \
        if (diff > 0) {                                                        \
            /* basically flexNext() up to the next element */                  \
            while (diff--) {                                                   \
                const size_t entryLen = flexRawEntryLength(fe_);               \
                /* Prefetch next entry to hide memory latency */               \
                __builtin_prefetch((fe_) + entryLen, 0, 1);                    \
                (fe_) += entryLen;                                             \
            }                                                                  \
        } else if (diff < 0) {                                                 \
            /* basically flexPrev() down to the prev element */                \
            while (diff++) {                                                   \
                flexEntry *prevEntry = flexGetPreviousEntry(fe_);              \
                /* Prefetch previous entry to hide memory latency */           \
                __builtin_prefetch(prevEntry, 0, 1);                           \
                (fe_) = prevEntry;                                             \
                assert((fe_) > (f));                                           \
            }                                                                  \
        } /* else, diff == 0 and we don't advance or retreat. */               \
                                                                               \
        (prevOffsetIndexPhysical) = offsetIndexPhysical;                       \
    } while (0)

/* Binary search a sorted flex */
DK_INLINE_ALWAYS flexEntry *abstractFindPositionByTypeSortedDirect(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth,
    const bool useReference, const multimapAtom *const referenceContainer,
    const bool useHighestInsertPosition, const bool useLowestMatch) {
    /* useLowestMatch: on a match keep searching lower, so among rows that
     * match 'compareElementDepth' elements the first one is returned */
    flexEntry *lowest = NULL;
    const uint_fast32_t count = flexCount_(f);
    const uint_fast32_t countValues = count / elementsPerEntry;
    size_t min = 0;
    size_t max = countValues;

    /* User provides a middle entry pointer, so we don't
     * have to iterate from 0 to the midpoint to start
     * comparing list elements; we can dive right in and
     * start with the midpoint given then decide grow/shrink/match. */
    flexEntry *fe = (flexEntry *)startMiddle;
    ssize_t prevOffsetIndexPhysical = ((min + max) >> 1) * elementsPerEntry;

#ifdef FLEX_DEBUG_EXTENSIVE
    /* Another expensive set of debugging checks.
     * Verifies our passed-in 'startMiddle' is *actual* middle of list.
     * (only valid if list has elements, so if no elements, ignore check) */
    assert(prevOffsetIndexPhysical
               ? flexIndexDirect(f, prevOffsetIndexPhysical) == startMiddle
               : true);
    assert(startMiddle);
    assert(elementsPerEntry > 0);

    /* Verify every element in the flex belongs to a unified "entry"
     * (if our flex has multi-element values).
     * (e.g. if values are each 2 elements (key/value),
     * we need an even number of elements in the entire list
     * or else the math below will break. */
    assert(count % elementsPerEntry == 0);
#endif

    while (min < max) {
        /* 'mid' is a logical element offset since our elements
         * are 'elementsPerEntry' apart (including the start element).
         *
         * We convert 'mid' into a physical flex index again by multiplying
         * 'mid' by 'elementsPerEntry'.
         *
         * So, 'elementsPerEntry' == 1 means every element gets tested:
         *     [0, 1, 2, 3, 4, 5, ...].
         *     'elementsPerEntry' == 2 means every 2nd element:
         *     [0, 2, 4, 6, 8, ...].
         *     'elementsPerEntry' == 3 means every 3rd element:
         *     [0, 3, 6, 9, ...]. */

        /* This math is safe since the performance of a flex would
         * break down if we had enough elements to overflow these entries.
         */
        const size_t mid = (min + max) >> 1;

        /* Move 'fe' up or down by following next/prev entries. */
        moveByOffsets_(mid, elementsPerEntry, fe, prevOffsetIndexPhysical);

        /* The probe is a plain box plus a stack slot.  Narrow elements
         * decode straight into the box and never touch 'wide'; a 96/128-bit
         * element (stored, or resolved through 'referenceContainer') lands
         * in 'wide', so databoxCompare always sees a full value. */
        databox box;
        databoxWide wide;

/* TODO: test speed difference of:
 *         - single key lookup
 *         - all element lookup.
 *       If noticeable, split out multiple element
 *       lookup into a new function. */
#if 0
        flexGetByType(fe, &box);

        /* FIXME: Compare currently only compares *matching* type
         * classes (i.e. we don't compare strings and integers).
         * At this point, we trust the user to only store same-type values
         * for proper comparison. */
        /* TODO: Allow user-defined comparison functions and/or well-define
         * ordering between all data types. */
        int compared = databoxCompare(&box, compareAgainst[0]);
        /* TODO: investigate tri-branching cmov with return capability */
        if (compared < 0) {
            /* current key < search box */
            min = mid + 1;
        } else if (compared > 0) {
            /* current key > search box */
            max = mid;
        } else {
            /* else, current key == search box, BUT
             *  - if this is a map alllowing duplicate keys,
             *    we *also* need to compare against the values so we
             *    sort in total order so we can find/remove across
             *    duplicate keys.
             *    Basically, if 'compareElementDepth' > 1, then
             *    treat 'compareElementDepth' elements after the key
             *    as also sortable, so they are also (logically) part
             *    of the sort key too.
             *    This also means if elements have an identical first key,
             *    they will still sort in order by their additional sub-values.
             *  - if the map doesn't allow duplicate keys, then
             *    jump to the else and return successfully found. */
            if (compareElementDepth > 1) {
                /* Found a key match, but need to compare against
                 * sub-elements too */

                const databox **walkingCompareAgainst = compareAgainst;
                flexEntry *walkingFE = fe;

                /* Binary search the field elements of this current key against
                 * the input elements for comparison */
                /* Note: 'compareElementDepth' is the number of *sub elements*
                 *       to compare *excluding* the original key position */
                for (uint32_t i = 1; i < compareElementDepth; i++) {
                    /* advance 'walkingFE' by one entry */
                    walkingFE += flexRawEntryLength(walkingFE);
                    flexGetByType(walkingFE, &box);

                    /* Compare sub-elements after key */
                    compared = databoxCompare(&box, walkingCompareAgainst[i]);
                    if (compared < 0) {
                        min = mid + 1;
                        break;
                    } else if (compared > 0) {
                        max = mid;
                        break;
                    } else {
                        /* else:
                         *   - return success if we are at max element.
                         *    - OR -
                         *   - loop again to process next element. */
                        if (i == compareElementDepth-1) {
                            *found = true;
                            return fe;
                        }
                    }
                }
            } else {
                *found = true;
                return fe;
            }
        }
#else
        flexEntry *walkingFE = fe;
        /* TODO: make this a do-while? */
        for (uint_fast32_t i = 0; i < compareElementDepth; i++) {
            const flexEncoding probed =
                flexDecode_(walkingFE, &box, &wide, FLEX_WIDE_STORE_SLOT_);
            if (useReference && FLEX_IS_REF_EXTERNAL(probed)) {
                flexResolveReferenceInto_(&box, &wide, referenceContainer);
            }

            const int compared = databoxCompare(&box, compareAgainst[i]);
            if (compared < 0) {
                /* current key < search box */
                min = mid + 1;
                break; /* break out of 'compareElementDepth' inner loop */
            }

            if (compared > 0) {
                /* current key > search box */
                max = mid;
                break; /* break out of 'compareElementDepth' inner loop */
            }

            /* down here, 'compared' == 0, so we found a match! */

            if (useHighestInsertPosition) {
                *found = true;
                min = mid + 1;
                break;
            }

            if (i == (compareElementDepth - 1)) {
                /* We found a complete element-by-element match at the
                 * full width of 'compareElementDepth', so we found
                 * the exact entry starting at 'fe'. */
                *found = true;
                if (useLowestMatch) {
                    lowest = fe;
                    max = mid;
                    break;
                }
                return fe;
            }

            /* compare next deeper element because we found
             * equal and have more elements to check against. */
            walkingFE += flexRawEntryLength(walkingFE);
        }
#endif
    }

    if (useHighestInsertPosition && *found) {
        return fe;
    }

    if (useLowestMatch && *found) {
        return lowest;
    }

    /* The binary search didn't find a match. */
    *found = false;

    if (min == countValues) {
        /* If min == countValues, we need to insert after
         * the tail of the list. */
        fe = FLEX_ENTRY_AFTER_TAIL(f);
    } else {
        /* else, we need to adjust 'fe' by one more value step. */
        moveByOffsets_(min, elementsPerEntry, fe, prevOffsetIndexPhysical);
    }

#ifdef FLEX_DEBUG_EXTENSIVE
    assert((size_t)(fe - f) <= flexTotalBytes_(f));
#endif

    return fe;
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedDirect_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, false, NULL, false, false);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedDirectHighest_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, false, NULL, true, false);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedWithReference_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth,
    const multimapAtom *referenceContainer) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, true, referenceContainer, false, false);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedWithReferenceHighest_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth,
    const multimapAtom *referenceContainer) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, true, referenceContainer, true, false);
}

flexEntry *flexFindByTypeSortedWithMiddleLowest(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE) {
    bool found = false;
    flexEntry *fe = abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1, false, NULL,
        false, true);
    return found ? fe : NULL;
}

flexEntry *flexFindByTypeSortedWithMiddleLowestWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const multimapAtom *referenceContainer) {
    bool found = false;
    flexEntry *fe = abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1, true,
        referenceContainer, false, true);
    return found ? fe : NULL;
}

/* Allow duplicate keys while inserting into map. */
bool flexInsertByTypeSortedWithMiddleMultiDirect(flex **const ff,
                                                 uint_fast32_t elementsPerEntry,
                                                 const databox *box[],
                                                 flexEntry **middleEntry) {
    return flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        ff, elementsPerEntry, box, middleEntry, false);
}

bool flexInsertByTypeSortedWithMiddle(flex **const ff, const databox *box,
                                      flexEntry **middleEntry) {
    const databox *boxes[1] = {box};
    return flexInsertByTypeSortedWithMiddleMultiDirect(ff, 1, boxes,
                                                       middleEntry);
}

void flexInsertByTypeSorted(flex **const ff, const databox *box) {
    bool found = false;
    flexEntry *const fe = flexFindPositionByTypeSortedDirect_(
        *ff, 1, &box, &found, flexMiddle(*ff, 1), 1);
    flexInsertByType(ff, fe, box);
}

void flexInsertByTypeSortedWithReference(
    flex **const ff, const databox *box,
    const multimapAtom *const referenceContainer) {
    bool found = false;
    flexEntry *const fe = flexFindPositionByTypeSortedWithReference_(
        *ff, 1, &box, &found, flexMiddle(*ff, 1), 1, referenceContainer);
    flexInsertByType(ff, fe, box);
}

/* If 'replace' is true:
 *   - if key is found, replace values. (no duplicate keys allowed)
 *   - if key is not found, insert new key and values.
 * Returns true if key already existed.
 * Returns false if key didn't exist and was inserted as new.
 * ALSO:
 * If 'newKeysBecomeShared' is true AND this is ONLY an insert (i.e. return
 * value is 'false'), then when key (box[0]) is DATABOX_BYTES, it can be turned
 * into an external pointer instead of being stored in-line inside the map. If
 * an external bytes allocation is created under these conditions, the returned
 * mdsc pointer is set inside '*keyAsAllocation' (and '*keyAsAllocation' is ONLY
 * valid if return value is 'false' because of INSERT — '*keyAsAllocation' is
 * never set during a replace). */
DK_INLINE_ALWAYS bool abstractInsertReplaceByTypeSortedWithMiddleMulti(
    flex **const ff, int_fast32_t elementsPerEntry, const databox **const box,
    flexEntry **const middleEntry, bool compareUsingKeyElementOnly,
    const bool useReference,
    const struct multimapAtom *const referenceContainer,
    const bool useSurrogateKeyForInsert, const databox *const surrogateKey,
    const bool useHighestInsertPosition, const bool newKeysBecomeShared,
    void **keyAsAllocation, const bool fullWidthDedupe,
    databox *const replacedOut) {
    /* When 'replacedOut' is non-NULL it holds 'elementsPerEntry - 1' slots
     * that receive the value entries a replace overwrites, in tuple order.
     * A call that does not replace (new key, full-width insert, set dedupe
     * hit, key-only map) reports VOID in every slot.  The slots may alias
     * the caller's value boxes (new values in, old values out), so they
     * are written only after every box has been encoded. */

    /* If no middle given, discover current middle (or head, if no
     * elements). */
    const size_t initialCount = flexCount_(*ff) / elementsPerEntry;
    flexEntry *mid = *middleEntry;
    if (!mid) {
        mid = flexMiddle(*ff, elementsPerEntry);
    }

#ifdef FLEX_DEBUG_EXTENSIVE
    /* Sanity check */
    assert(flexCount(*ff) % elementsPerEntry == 0);

    /* Slows down testing too much when enabled. */
    assert(initialCount ? mid == flexMiddle(*ff, elementsPerEntry) : true);
#endif

    /* We always use box[0] as initial search key */
    bool found = false;

    /* If 'compareUsingKeyElementOnly' then only compare *key*,
     * else compare key *and* values for proper insert sort position. */
    /* ALSO: using !compareUsingKeyElementOnly imples we ALLOW DUPLICATE
     *       FULL WIDTH ENTRIES AS A VALID AND SUPPORTED USE CASE! */
    const uint_fast32_t maxSubElementsToCompare =
        compareUsingKeyElementOnly ? 1 : elementsPerEntry;

    flexEntry *fe = NULL;
    if (useReference) {
        if (useHighestInsertPosition) {
            fe = flexFindPositionByTypeSortedWithReferenceHighest_(
                *ff, elementsPerEntry, box, &found, mid,
                maxSubElementsToCompare, referenceContainer);
        } else {
            fe = flexFindPositionByTypeSortedWithReference_(
                *ff, elementsPerEntry, box, &found, mid,
                maxSubElementsToCompare, referenceContainer);
        }
    } else {
        if (useHighestInsertPosition) {
            fe = flexFindPositionByTypeSortedDirectHighest_(
                *ff, elementsPerEntry, box, &found, mid,
                maxSubElementsToCompare);
        } else {
            fe = flexFindPositionByTypeSortedDirect_(*ff, elementsPerEntry, box,
                                                     &found, mid,
                                                     maxSubElementsToCompare);
        }
    }

    /* TRUE SET SEMANTICS: full-width compare AND no duplicate entry.
     * The plain full-width mode below keeps its historical
     * duplicates-allowed behavior (sorted-bag consumers such as
     * InsertFullWidth rely on it); set-mode multimaps route through
     * this dedupe instead — a found full-width-equal entry is
     * reported found WITHOUT touching the flex.  Without this, every
     * re-insert of an existing set member appended a duplicate row
     * while returning 'found', so the caller's reply looked correct
     * as the structure silently diverged. */
    if (fullWidthDedupe && found) {
        for (int_fast32_t i = 0; replacedOut && i < elementsPerEntry - 1; i++) {
            replacedOut[i] = (databox){.type = DATABOX_VOID};
        }
        return true;
    }

    /* Only replace if we *found* the key.
     * If so, replace everything after the key.  */
    const bool actuallyReplace = compareUsingKeyElementOnly && found;
    const bool isFullWidthReplace = !compareUsingKeyElementOnly;

    /* Most use cases don't have many entries: stack scratch covers
     * multimap arity up to 8 (matching COMMON_INSERT_MAX_CONTENTS_),
     * so common sorted inserts never allocate. */
    enum { SORTED_INSERT_STACK_ELEMENTS_ = 8 };
    databox copyBox_[SORTED_INSERT_STACK_ELEMENTS_];
    flexInsertContents content_[SORTED_INSERT_STACK_ELEMENTS_];
    flexInsertContents *contents_[SORTED_INSERT_STACK_ELEMENTS_];
    __int128_t bigBuffer_[SORTED_INSERT_STACK_ELEMENTS_];

    databox *copyBox = copyBox_;
    flexInsertContents *content = content_;
    flexInsertContents **contents = contents_;
    __int128_t *bigBuffer = bigBuffer_;

    /* Wider entries spill to one heap block carved in descending
     * alignment order: bigBuffer (16-byte alignment) first, then the
     * 16-byte copyBox structs, then content, then the pointer array.
     * Every region must be disjoint - a previous carve based bigBuffer
     * at the wrong offset, overlapping the tail of content[], which
     * silently corrupted insert descriptors whenever a wide entry
     * carried 128-bit elements. Block frees itself at scope exit. */
    cleaner(zfree) void *sortedInsertScratch_ = NULL;
    if (elementsPerEntry > SORTED_INSERT_STACK_ELEMENTS_) {
        const size_t bigBufferLen = elementsPerEntry * sizeof(*bigBuffer);
        const size_t copyLen = elementsPerEntry * sizeof(*copyBox);
        const size_t contentLen = elementsPerEntry * sizeof(*content);
        const size_t contentsLen = elementsPerEntry * sizeof(*contents);

        sortedInsertScratch_ =
            zmalloc(bigBufferLen + copyLen + contentLen + contentsLen);
        uint8_t *blockBytes = (uint8_t *)sortedInsertScratch_;
        bigBuffer = (__int128_t *)blockBytes;
        copyBox = (databox *)(blockBytes + bigBufferLen);
        content = (flexInsertContents *)(blockBytes + bigBufferLen + copyLen);
        contents = (flexInsertContents **)(blockBytes + bigBufferLen + copyLen +
                                           contentLen);
    }

    if (!found || isFullWidthReplace) {
        /* Establish insert contents of box[0] as required */
        if (useSurrogateKeyForInsert) {
            copyBox[0] = *surrogateKey;
            insertContentsFromBox(&copyBox[0], &content[0],
                                  CONVERSION_OVERRIDE_NONE, 0, bigBuffer);
        } else if (newKeysBecomeShared) {
            copyBox[0] = *box[0];
            /* TODO: allow parameterization of conversion override option?
             *       We have multiple override options, but no way for users
             *       to access them unless we surface the option higher. */
            if (insertContentsFromBox(
                    &copyBox[0], &content[0],
                    CONVERSION_OVERRIDE_BECOME_MDSC_BYTES_GT12, 0, bigBuffer)) {
                *keyAsAllocation = copyBox[0].data.ptr;
            } else {
#if 0
            /* NOTE: for signed integers this will report one higher than the
             *       expected value due to integer encoding.
             *       (e.g. -12345 will report -12344 because we offset all
             *        negative numbers by 1 since we don't need to store
             *        a signed zero (so, -1 is stored as 0, -2 as -1, ...) */
            databoxReprSay("Did not convert on insert for:", &copyBox[0]);
#endif
            }
        } else {
            copyBox[0] = *box[0];
            insertContentsFromBox(&copyBox[0], &content[0],
                                  CONVERSION_OVERRIDE_NONE, 0, bigBuffer);
        }
    } else {
        /* If we aren't setting zero, we need to zero out the zero data. */
        /* TODO: fix flexInsert* common macros to not assume content[0] will
         *       be populated when we are doing an offset-replace */
        content[0] = (flexInsertContents){0};
    }

    /* Now assemble remaining common insert contents */
    /* We need to populate contents[0] because of commonality
     * inside commonFlexInsertCalculatePreconditions_() whether it
     * gets used or not. TODO: refactor commonFlexInsertCalculatePreconditions_
     * to not require always checking content[0] because we don't need it
     * for things like replace() from here. */
    /* TODO: move contents[0] inside the if statement above after we
     *       refactor commonFlexInsertCalculatePreconditions_() */
    contents[0] = &content[0];
    for (int_fast32_t i = 1; i < elementsPerEntry; i++) {
        copyBox[i] = *box[i];
        insertContentsFromBox(&copyBox[i], &content[i],
                              CONVERSION_OVERRIDE_NONE, i, bigBuffer);
        contents[i] = &content[i];
    }

    /* Every value box is encoded, so 'replacedOut' may now overwrite
     * them. */
    if (replacedOut) {
        for (int_fast32_t i = 0; i < elementsPerEntry - 1; i++) {
            replacedOut[i] = (databox){.type = DATABOX_VOID};
        }
    }

    flexHeaderInfo headerDiff = {0};
    const bool insertedKeyBeforeCurrentMiddle = fe < mid;
    const size_t offset = mid - *ff;

    /* Here, EITHER:
     *  - we FOUND an existing element and we are REPLACING VALUES (no key)
     *  - we FOUND an existing FULL WIDTH element and are INSERTING DUPLICATE
     *    VALUES (the 'else')
     *  - we DID NOT FIND any existing value and are INSERTING NEW VALUES (the
     *    'else') */
    if (actuallyReplace) {
        /* Only replace if we have values.  If 'elementsPerEntry' is 1,
         * then we only have keys and it makes no sense to replace a
         * found key with itself.
         * Don't make any changes and just return successfully found. */
        if (elementsPerEntry == 1) {
            return true;
        }

        /* Capture the old values before the replace rewrites them.  A
         * 96/128-bit integer does not fit a databox, so its slot reports
         * only the type (payload pointer NULL). */
        if (replacedOut) {
            flexEntry *prior = fe;
            for (int_fast32_t i = 0; i < elementsPerEntry - 1; i++) {
                prior = flexNext(*ff, prior);
                assert(prior);
                /* The slot was reset above, so its 'big' is clear and a
                 * wide value decodes type-only. */
                flexGetByType(prior, &replacedOut[i]);
            }
        }

        /* 1 below because the replacement is *after* the key.
         * We don't need to overwrite the key since we already found
         * it, we know it exists, and 'fe' is pointing right at it. */
        flexInsertReplace_____(ff, fe, contents, elementsPerEntry, 1,
                               &headerDiff);
    } else {
        flexInsert_____(ff, fe, contents, elementsPerEntry, &headerDiff);
    }

    /* (heap scratch, if any, frees itself at scope exit) */

    /* Preliminarily update midpoint given new header offset details */
    mid = *ff + offset + headerDiff.headerDiff;

    /* If inserted before middle, we need to alter saved middle
     * offset by the insert size difference. */
    if (insertedKeyBeforeCurrentMiddle) {
        mid += headerDiff.insertedBytes;
    }

    /* If replacing the entry (because it was found), midpoint doesn't
     * change, it just has its offset updated based on inserted contents. */
    if (actuallyReplace) {
#ifdef FLEX_DEBUG_EXTENSIVE
        assert(mid == flexMiddle(*ff, 2));
#endif
        *middleEntry = mid;
    } else {
        /* else, we inserted an entirely new entry and need to calculate
         * a new middle. */
        const bool atEvenToOddTransitionBoundary = initialCount % 2 == 0;
        /* Move middle *DOWN* by one entry if insertedMax entry is *before*
         * middle.
         * (i.e. inserting before middle means the new middle element is the
         * value immediately previous to the old middle) */
        if (insertedKeyBeforeCurrentMiddle) {
            /* We only move middle *down* if we are doing an even->odd
             * transition.
             * (i.e. the actual offset count isn't changing, so we need to
             * maintain it by backing up by one whole value) */
            if (atEvenToOddTransitionBoundary) {
                /* If inserted before middle, move middle down by one value.
                 */
                while (elementsPerEntry--) {
                    mid = flexGetPreviousEntry(mid);
                }
            }
        } else {
            /* Because integer division, we only move middle up if new count
             * is EVEN (meaning start count is ODD)
             * (e.g. 0 / 2 = 0; 1 / 2 = 0; 2 / 2 = 1; 3 / 2 = 1; ...
             *       midpoint only increases when we transition from odd
             *       count to an even count, else midpoint doesn't change
             *       because of floor integer division.) */
            if (!atEvenToOddTransitionBoundary) {
                /* If inserted after middle, move middle up by one element
                 * group. */
                while (elementsPerEntry--) {
                    mid += flexRawEntryLength(mid);
                }
            }
        }

        *middleEntry = mid;
    }

    return found;
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        false, NULL, false, NULL, false, false, NULL, false, NULL);
}

/* As above, also reporting replaced values (see flex.h). */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly, databox *const replacedOut) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        false, NULL, false, NULL, false, false, NULL, false, replacedOut);
}

/* TRUE SET insert: full-width compare AND dedupe — a found
 * full-width-equal entry is a no-op that returns true.  This is what
 * mapIsSet multimaps mean by "set"; the plain Direct call above with
 * compareUsingKeyElementOnly=false is the duplicates-allowed sorted
 * bag (InsertFullWidth). */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, false, false, NULL, false, NULL,
        false, false, NULL, true, NULL);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly, void **recoveredPointer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        false, NULL, false, NULL, false, true, recoveredPointer, false, NULL);
}

/* As above, also reporting replaced values (see flex.h). */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly, void **recoveredPointer,
    databox *const replacedOut) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        false, NULL, false, NULL, false, true, recoveredPointer, false,
        replacedOut);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        true, referenceContainer, false, NULL, false, false, NULL, false, NULL);
}

/* TRUE SET insert with atom references — the reference-container
 * sibling of the DirectSet variant above. */
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceSet(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const struct multimapAtom *const referenceContainer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, false, true, referenceContainer,
        false, NULL, false, false, NULL, true, NULL);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, const databox *const boxInsertKey,
    flexEntry **const middleEntry, const bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        true, referenceContainer, true, boxInsertKey, false, false, NULL, false,
        NULL);
}

void flexAppendMultipleExact(flex **ff, const uint_fast32_t elementsPerEntry,
                             const databox *const *const box) {
    if (elementsPerEntry == 0) {
        return;
    }

    /* Fixed stack scratch for common widths; wider appends spill to
     * one heap block (alignment-descending carve, scope-exit freed) so
     * arbitrary widths can't blow the stack via VLAs. */
    enum { APPEND_STACK_ELEMENTS_ = 8 };
    databox copyBoxStack[APPEND_STACK_ELEMENTS_];
    flexInsertContents contentStack[APPEND_STACK_ELEMENTS_];
    flexInsertContents *contentsStack[APPEND_STACK_ELEMENTS_];
    __int128_t bigBufferStack[APPEND_STACK_ELEMENTS_];

    databox *copyBox = copyBoxStack;
    flexInsertContents *content = contentStack;
    flexInsertContents **contents = contentsStack;
    __int128_t *bigBuffer = bigBufferStack;

    cleaner(zfree) void *appendScratch_ = NULL;
    if (elementsPerEntry > APPEND_STACK_ELEMENTS_) {
        const size_t bigBufferLen = elementsPerEntry * sizeof(*bigBuffer);
        const size_t copyLen = elementsPerEntry * sizeof(*copyBox);
        const size_t contentLen = elementsPerEntry * sizeof(*content);
        const size_t contentsLen = elementsPerEntry * sizeof(*contents);

        appendScratch_ =
            zmalloc(bigBufferLen + copyLen + contentLen + contentsLen);
        uint8_t *blockBytes = (uint8_t *)appendScratch_;
        bigBuffer = (__int128_t *)blockBytes;
        copyBox = (databox *)(blockBytes + bigBufferLen);
        content = (flexInsertContents *)(blockBytes + bigBufferLen + copyLen);
        contents = (flexInsertContents **)(blockBytes + bigBufferLen + copyLen +
                                           contentLen);
    }

    for (uint_fast32_t i = 0; i < elementsPerEntry; i++) {
        /* A NULL value is stored as null, as the pushes store it */
        copyBox[i] = box[i] ? *box[i] : DATABOX_BOX_NULL;
        insertContentsFromBox(&copyBox[i], &content[i],
                              CONVERSION_OVERRIDE_NONE, i, bigBuffer);
        contents[i] = &content[i];
    }

    flexInsert_____(ff, FLEX_ENTRY_END(*ff), contents, elementsPerEntry, NULL);
}

void flexAppendMultiple(flex **ff, const uint_fast32_t elementsPerEntry,
                        const databox *const *const box) {
    /* The auto path differs from the exact one only for strings that spell
     * a number (databoxCanonical, as flexInsertBytes applies it), so swap
     * those for their canonical boxes and append exactly.  A canonical
     * 96/128-bit result lives in its scratch slot, so rows are appended a
     * chunk at a time; a row of at most one chunk is one reallocation. */
    enum { APPEND_AUTO_CHUNK_ = 32 };
    databoxBig scratch[APPEND_AUTO_CHUNK_];
    const databox *chunk[APPEND_AUTO_CHUNK_];
    uint_fast32_t n = 0;
    for (uint_fast32_t i = 0; i < elementsPerEntry; i++) {
        chunk[n] = box[i] ? databoxCanonical(box[i], &scratch[n]) : NULL;
        if (++n == APPEND_AUTO_CHUNK_) {
            flexAppendMultipleExact(ff, n, chunk);
            n = 0;
        }
    }

    if (n) {
        flexAppendMultipleExact(ff, n, chunk);
    }
}

/* The row's boxes as the pointer array the appends take */
static void flexRowPointers_(const flexRow *r,
                             const databox *at[FLEX_ROW_MAX]) {
    for (uint32_t i = 0; i < r->n; i++) {
        at[i] = &r->box[i];
    }
}

void flexRowAppend(flex **ff, flexRow *r) {
    const databox *at[FLEX_ROW_MAX];
    flexRowPointers_(r, at);
    flexAppendMultiple(ff, r->n, at);
    r->n = 0;
}

void flexRowAppendExact(flex **ff, flexRow *r) {
    const databox *at[FLEX_ROW_MAX];
    flexRowPointers_(r, at);
    flexAppendMultipleExact(ff, r->n, at);
    r->n = 0;
}

int flexCompareEntries(const flex *const f,
                       const databox *const *const elements,
                       const uint_fast32_t elementsPerEntry,
                       const int_fast32_t offset) {
    flexEntry *fCompare = flexIndex(f, offset * elementsPerEntry);
    /* A plain box plus a stack slot, as in the sorted-search probe. */
    databox box;
    databoxWide wide;

    for (uint_fast32_t i = 0; i < elementsPerEntry; i++) {
        flexDecode_(fCompare, &box, &wide, FLEX_WIDE_STORE_SLOT_);
        const int compared = databoxCompare(&box, elements[i]);

        /* If equal and not at last element, check next deeper element. */
        if (compared == 0 && ((i + 1) < elementsPerEntry)) {
            fCompare += flexRawEntryLength(fCompare);
        } else {
            /* else, just return the comparator */
            return compared;
        }
    }

    __builtin_unreachable();
}

flexEntry *flexFindByTypeSorted(const flex *const f,
                                const uint_fast32_t elementsPerEntry,
                                const databox *compareAgainst) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedDirect_(
        f, elementsPerEntry, &compareAgainst, &found,
        flexMiddle(f, elementsPerEntry), 1);

    return found ? fe : NULL;
}

flexEntry *flexFindByTypeSortedFullWidth(const flex *const f,
                                         const uint_fast32_t elementsPerEntry,
                                         const databox **const compareAgainst) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedDirect_(
        f, elementsPerEntry, compareAgainst, &found,
        flexMiddle(f, elementsPerEntry), elementsPerEntry);

    return found ? fe : NULL;
}

flexEntry *flexGetByTypeSortedWithMiddle(const flex *const f,
                                         const uint_fast32_t elementsPerEntry,
                                         const databox *compareAgainst,
                                         const flexEntry *middleFE) {
    bool found = false;
    return flexFindPositionByTypeSortedDirect_(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1);
}

flexEntry *flexGetByTypeSortedWithMiddleWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const multimapAtom *referenceContainer) {
    bool found = false;
    return flexFindPositionByTypeSortedWithReference_(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1,
        referenceContainer);
}

flexEntry *flexFindByTypeSortedWithMiddle(const flex *const f,
                                          const uint_fast32_t elementsPerEntry,
                                          const databox *compareAgainst,
                                          const flexEntry *middleFE) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedDirect_(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1);

    return found ? fe : NULL;
}

flexEntry *flexFindByTypeSortedWithMiddleGetEntry(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedDirect_(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1);

    return fe;
}

flexEntry *flexFindByTypeSortedWithMiddleWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const multimapAtom *referenceContainer) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedWithReference_(
        f, elementsPerEntry, &compareAgainst, &found, middleFE, 1,
        referenceContainer);
#if 0
        printf("Got FLEX entry: %p\n", fe);
#endif

    return found ? fe : NULL;
}

flexEntry *flexFindByTypeSortedWithMiddleFullWidth(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **const compareAgainst, const flexEntry *middleFE) {
    bool found = false;
    flexEntry *fe =
        flexFindPositionByTypeSortedDirect_(f, elementsPerEntry, compareAgainst,
                                            &found, middleFE, elementsPerEntry);

    return found ? fe : NULL;
}

flexEntry *flexFindByTypeSortedWithMiddleFullWidthWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **const compareAgainst, const flexEntry *middleFE,
    const multimapAtom *referenceContainer) {
    bool found = false;
    flexEntry *fe = flexFindPositionByTypeSortedWithReference_(
        f, elementsPerEntry, compareAgainst, &found, middleFE, elementsPerEntry,
        referenceContainer);

    return found ? fe : NULL;
}

#define flexFind__(...)                                                        \
    do {                                                                       \
        return flexFind_(f, fe, skip, __VA_ARGS__);                            \
    } while (0)

#define flexFindForward_(...) flexFind__(true, __VA_ARGS__)
#define flexFindReverse_(...) flexFind__(false, __VA_ARGS__)

flexEntry *flexFindSigned(const flex *const f, flexEntry *const fe,
                          const int64_t val, const uint32_t skip) {
    flexFindForward_((uint8_t *)&val, sizeof(val), flexEntryCompareSigned__);
}

flexEntry *flexFindSignedReverse(const flex *const f, flexEntry *const fe,
                                 const int64_t val, const uint32_t skip) {
    flexFindReverse_((uint8_t *)&val, sizeof(val), flexEntryCompareSigned__);
}

flexEntry *flexFindUnsigned(const flex *const f, flexEntry *const fe,
                            const uint64_t val, const uint32_t skip) {
    flexFindForward_((uint8_t *)&val, sizeof(val), flexEntryCompareUnsigned__);
}

flexEntry *flexFindUnsignedReverse(const flex *const f, flexEntry *const fe,
                                   const uint64_t val, const uint32_t skip) {
    flexFindReverse_((uint8_t *)&val, sizeof(val), flexEntryCompareUnsigned__);
}

flexEntry *flexFindString(const flex *const f, flexEntry *const fe,
                          const void *val, const size_t len,
                          const uint32_t skip) {
    flexFindForward_((const uint8_t *)val, len, flexEntryCompareString__);
}

flexEntry *flexFindStringReverse(const flex *const f, flexEntry *const fe,
                                 const void *val, const size_t len,
                                 const uint32_t skip) {
    flexFindReverse_((const uint8_t *)val, len, flexEntryCompareString__);
}

/* Typed match for the probe kinds without a dedicated comparator: 'src' is
 * the probe databox.  A number matches a numeric entry of the same value
 * (identity order, so a NaN probe finds a NaN entry); TRUE, FALSE and NULL
 * match only their own type.  Strings never match here. */
DK_STATIC bool flexEntryCompareBox__(const flexEntryData *entry,
                                     const uint8_t *src, const size_t len) {
    DK_NOTUSED(len);
    const databox *const probe = (const databox *)src;
    if (FLEX_IS_STR(entry->encoding) || entry->encoding == FLEX_BYTES_EMPTY) {
        return false;
    }

    databox got = {{0}};
    databoxWide slot;
    flexGetByTypeInto(entry->fe, &got, &slot);
    const bool probeNumeric = DATABOX_IS_NUMERIC(probe) ||
                              probe->type == DATABOX_SIGNED_128 ||
                              probe->type == DATABOX_UNSIGNED_128;
    if (probeNumeric) {
        const bool gotNumeric = DATABOX_IS_NUMERIC(&got) ||
                                got.type == DATABOX_SIGNED_128 ||
                                got.type == DATABOX_UNSIGNED_128;
        return gotNumeric && databoxCompare(&got, probe) == 0;
    }

    return got.type == probe->type;
}

DK_STATIC flexEntry *flexFindByTypeDirectional_(flex *const f,
                                                flexEntry *const fe,
                                                uint32_t skip, bool forward,
                                                const databox *box) {
    switch (box->type) {
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
        flexFind__(forward, databoxBytes(box), databoxLen(box),
                   flexEntryCompareString__);
        return NULL; /* flexFind__ macro returns, but this satisfies static
                        analyzers */
    case DATABOX_SIGNED_64:
        flexFind__(forward, (uint8_t *)&box->data.i64, sizeof(box->data.i64),
                   flexEntryCompareSigned__);
        return NULL; /* flexFind__ macro returns, but this satisfies static
                        analyzers */
    case DATABOX_UNSIGNED_64:
        flexFind__(forward, (uint8_t *)&box->data.u64, sizeof(box->data.u64),
                   flexEntryCompareUnsigned__);
        return NULL; /* flexFind__ macro returns, but this satisfies static
                        analyzers */
    case DATABOX_FLOAT_32:
    case DATABOX_DOUBLE_64:
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
        if ((box->type == DATABOX_SIGNED_128 ||
             box->type == DATABOX_UNSIGNED_128) &&
            databoxIsWideDeferred(box)) {
            return NULL; /* a wide probe with no payload has no value */
        }

        flexFind__(forward, (const uint8_t *)box, sizeof(*box),
                   flexEntryCompareBox__);
        return NULL; /* flexFind__ macro returns, but this satisfies static
                        analyzers */
    default:
        /* No find semantics for this type (containers, pointers): no
         * match, never the flex itself. */
        return NULL;
    }
}

/* Forward */
flexEntry *flexFindByTypeHead(flex *f, const databox *box, uint32_t skip) {
    return flexFindByTypeDirectional_(f, flexHead(f), skip, true, box);
}

flexEntry *flexFindByType(flex *f, flexEntry *fe, const databox *box,
                          uint32_t skip) {
    return flexFindByTypeDirectional_(f, fe, skip, true, box);
}

/* Reverse */
flexEntry *flexFindByTypeReverse(flex *f, flexEntry *fe, const databox *box,
                                 uint32_t skip) {
    return flexFindByTypeDirectional_(f, fe, skip, false, box);
}

/* What flexFind looks for: the caller's bytes, and their canonical number
 * when they spell one. */
typedef struct flexFindProbe_ {
    const uint8_t *bytes;
    size_t len;
    const databox *number; /* NULL when the bytes are not a numeral */
} flexFindProbe_;

DK_STATIC bool flexEntryCompareProbe__(const flexEntryData *entry,
                                       const uint8_t *src, const size_t len) {
    DK_NOTUSED(len);
    const flexFindProbe_ *const probe = (const flexFindProbe_ *)src;

    /* A string entry matches exactly the same bytes.  The empty string has
     * an encoding of its own. */
    if (entry->encoding == FLEX_BYTES_EMPTY) {
        return probe->len == 0;
    }

    if (FLEX_IS_STR(entry->encoding)) {
        return entry->len == probe->len &&
               (probe->len == 0 ||
                memcmp(FLEX_ENTRY_DATA(entry), probe->bytes, probe->len) == 0);
    }

    /* A numeric entry matches a numeral of the same value. */
    if (!probe->number) {
        return false;
    }

    databox got = {{0}};
    databoxWide slot;
    flexGetByTypeInto(entry->fe, &got, &slot);
    const bool numeric = DATABOX_IS_NUMERIC(&got) ||
                         got.type == DATABOX_SIGNED_128 ||
                         got.type == DATABOX_UNSIGNED_128;
    return numeric && databoxCompare(&got, probe->number) == 0;
}

flexEntry *flexFind(flex *f, const void *vstr, uint32_t vlen, uint32_t skip) {
    const databox bytes = {
        .data.bytes.custart = vstr, .type = DATABOX_BYTES, .len = vlen};
    databoxBig scratch;
    const databox *const canonical = databoxCanonical(&bytes, &scratch);
    const flexFindProbe_ probe = {
        .bytes = vstr,
        .len = vlen,
        .number = canonical == &bytes ? NULL : canonical,
    };

    return flexFind_(f, flexHead(f), skip, true, (const uint8_t *)&probe,
                     sizeof(probe), flexEntryCompareProbe__);
}

/* ====================================================================
 * flex element count retrieval
 * ==================================================================== */
size_t flexCount(const flex *f) {
    return flexCount_(f);
}

bool flexIsEmpty(const flex *f) {
    return flexCount_(f) == 0;
}

/* ====================================================================
 * flex physical size
 * ==================================================================== */
size_t flexBytes(const flex *f) {
    return flexTotalBytes_(f);
}

size_t flexBytesLength(const flex *f) {
    return FLEX_TOTAL_BYTES_WIDTH(f);
}

/* A flex is one self-describing buffer (header, entries, trailers), so
 * physical equality of metadata and entries is byte equality.  The
 * header's total-bytes field rejects a size mismatch before memcmp. */
bool flexEqual(const flex *a, const flex *b) {
    if (a == b) {
        return true;
    }

    if (!a || !b) {
        return false;
    }

    const size_t bytes = flexBytes(a);
    return bytes == flexBytes(b) && memcmp(a, b, bytes) == 0;
}

/* ====================================================================
 * Untrusted-input validation
 * ====================================================================
 * The validators must return for any input (see flex.h).  They share
 * encoding tables and decode macros with the decode path but not its
 * asserting helpers:
 *   - each encoding byte is classified by flexEntryClassify, which
 *     returns UNKNOWN for any value no writer emits;
 *   - each split varint is decoded by flexSplitVarintTryDecode, which
 *     bounds the varint's width by the bytes remaining and rejects
 *     external widths a writer never emits;
 *   - every length is checked against the bytes remaining before
 *     anything behind it is read. */

/* Decode one split-full-no-zero varint (header total-bytes, string
 * length, container inner length) from 'p' with 'remaining' bytes
 * available.  Returns false if the varint's width exceeds 'remaining'
 * or its external width is outside BYTE_2..BYTE_8.  On success stores
 * the width and value.  The value comes from the decode path's own
 * macro, so validate and decode agree on every host; it wraps exactly
 * as the decode macro's does, and callers bound it like any other
 * length.
 *
 * Bits 4-5 of an external tag are not checked: the decode macro takes
 * the width from the low nibble alone, so it reads such a tag the same
 * way. */
DK_INLINE_ALWAYS bool flexSplitVarintTryDecode(const uint8_t *const p,
                                               const size_t remaining,
                                               varintWidth *const outWidth,
                                               uint64_t *const outVal) {
    if (remaining == 0) {
        return false;
    }

    /* Reads byte 0 only, which 'remaining' already covers. */
    const size_t width = varintSplitFullNoZeroGetLenQuick_(p);
    if (width > remaining) {
        return false;
    }

    if (varintSplitFullNoZeroEncoding2_(p) == VARINT_SPLIT_FULL_NO_ZERO_VAR) {
        /* BYTE_1 is never written; a tag naming it is corrupt.  Widths
         * above BYTE_8 do not fit the decode's uint64_t loader, which
         * asserts on them. */
        const varintWidth external =
            varintSplitFullNoZeroEncodingWidthBytesExternal_(p);
        if (external < (varintWidth)(VARINT_SPLIT_FULL_NO_ZERO_BYTE_2 -
                                     VARINT_SPLIT_FULL_NO_ZERO_VAR) ||
            external > (varintWidth)(VARINT_SPLIT_FULL_NO_ZERO_BYTE_8 -
                                     VARINT_SPLIT_FULL_NO_ZERO_VAR)) {
            return false;
        }
    }

    varintWidth decodedWidth = 0;
    uint64_t decoded = 0;
    varintSplitFullNoZeroGet_(p, decodedWidth, decoded);

    /* Both macros read the same tag bits; fail closed if they ever
     * disagree. */
    if ((size_t)decodedWidth != width) {
        return false;
    }

    *outWidth = decodedWidth;
    *outVal = decoded;
    return true;
}

bool flexValidateBuffer(const uint8_t *buf, size_t buflen) {
    /* Minimum: 2 bytes (1 byte split varint for total_bytes that
     * encodes a small value, plus 1 byte for the count varint).  Any
     * shorter input cannot be a valid flex envelope. */
    if (!buf || buflen < FLEX_EMPTY_HEADER_SIZE) {
        return false;
    }

    /* Checked decode of the leading total-bytes varint; its tag byte is
     * untrusted. */
    varintWidth total_bytes_encoded_width = 0;
    uint64_t total_bytes_claim = 0;
    if (!flexSplitVarintTryDecode(buf, buflen, &total_bytes_encoded_width,
                                  &total_bytes_claim)) {
        return false;
    }
    const size_t total_bytes_width = (size_t)total_bytes_encoded_width;

    /* The flex header self-describes the buffer's total length.  For
     * a well-formed buffer that length must equal the input length;
     * any other value means the input is truncated or padded with
     * garbage neither end can interpret consistently. */
    if (total_bytes_claim != (uint64_t)buflen) {
        return false;
    }

    /* Count varint immediately follows the total-bytes varint.  Need
     * at least 1 byte for it. */
    if (total_bytes_width >= buflen) {
        return false;
    }

    return true;
}

bool flexPeekTotalBytes(const uint8_t *buf, size_t buflen, size_t *out_total) {
    /* Mirror flexValidateBuffer's leading-varint extraction but return
     * the claim INSTEAD OF requiring it match buflen — callers that
     * embed a flex as a strict prefix of a larger buffer need this
     * primitive so they can advance past the prefix to a trailing
     * opaque region (e.g. clusterMailbox's inner-header + payload
     * framing). */
    if (buf == NULL || out_total == NULL || buflen < FLEX_EMPTY_HEADER_SIZE) {
        return false;
    }
    /* Same checked decode as flexValidateBuffer. */
    varintWidth total_bytes_encoded_width = 0;
    uint64_t total_bytes_claim = 0;
    if (!flexSplitVarintTryDecode(buf, buflen, &total_bytes_encoded_width,
                                  &total_bytes_claim)) {
        return false;
    }
    const size_t total_bytes_width = (size_t)total_bytes_encoded_width;
    /* Claim must fit in the outer buffer — otherwise a caller-supplied
     * follow-up flexValidateBuffer(buf, claim) would read past the end. */
    if (total_bytes_claim > (uint64_t)buflen) {
        return false;
    }
    /* Claim must also leave at least one byte for the trailing count
     * varint, matching flexValidateBuffer's invariant. */
    if (total_bytes_claim <= (uint64_t)total_bytes_width) {
        return false;
    }
    *out_total = (size_t)total_bytes_claim;
    return true;
}

/* Class of an entry's encoding byte, for the validate walk.  Every byte
 * value maps to one class; UNKNOWN covers FLEX_SAME, the reserved
 * container tags and the gap before FLEX_SAME, FLEX_REAL_32D/64D, and
 * the MDSC references when mdsc support is compiled out. */
typedef enum flexEntryClass {
    FLEX_ENTRY_CLASS_UNKNOWN = 0,
    FLEX_ENTRY_CLASS_IMMEDIATE,
    FLEX_ENTRY_CLASS_STRING,
    FLEX_ENTRY_CLASS_CONTAINER,
    FLEX_ENTRY_CLASS_FIXED
} flexEntryClass;

/* Classify one encoding byte.  On FIXED, *outFixedSize receives the
 * entry's payload byte count (the class whose payload is a function of
 * the encoding byte alone).  Reads nothing but the byte handed in. */
DK_INLINE_ALWAYS flexEntryClass flexEntryClassify(const flexEncoding encoding,
                                                  size_t *const outFixedSize) {
    if (FLEX_IS_IMMEDIATE(encoding)) {
        return encoding == FLEX_SAME ? FLEX_ENTRY_CLASS_UNKNOWN
                                     : FLEX_ENTRY_CLASS_IMMEDIATE;
    }

    if (FLEX_IS_STR(encoding)) {
        return FLEX_ENTRY_CLASS_STRING;
    }

    switch (encoding) {
    case FLEX_CONTAINER_MAP:
    case FLEX_CONTAINER_LIST:
    case FLEX_CONTAINER_SET:
    case FLEX_CONTAINER_TUPLE:
        return FLEX_ENTRY_CLASS_CONTAINER;
    default:
        break;
    }

    /* Everything left is either sizable from the encoding byte alone or
     * not sizable at all, and the payload-width table answers which. */
    return flexFixedWidthPayloadSize(encoding, outFixedSize)
               ? FLEX_ENTRY_CLASS_FIXED
               : FLEX_ENTRY_CLASS_UNKNOWN;
}

/* Decode the length-of-this-entry for the entry at `fe`, where `remaining`
 * is the number of bytes available starting at `fe` (i.e. the byte budget
 * between `fe` and the end of the buffer).  Sets *out_entry_len to the
 * total number of bytes the entry consumes (encoding header + payload +
 * reverse-encoding trailer) on success.  Returns false if any required
 * byte read would exceed `remaining`, or if the entry's declared total
 * size does not fit in `remaining`.
 *
 * This is the bounds-safe twin of `flexRawEntryLength` — the latter
 * trusts its input and can over-read up to 16 bytes when fed a corrupted
 * string-length varint.  This helper is the safe-walk primitive for
 * flexValidateBufferDeep. */
DK_INLINE_ALWAYS bool flexRawEntryLengthSafe(const flexEntry *const fe,
                                             const size_t remaining,
                                             size_t *const out_entry_len) {
    /* Need at least the encoding byte. */
    if (remaining == 0) {
        return false;
    }

    const flexEncoding encoding = fe[0];
    size_t fixedPayload = 0;
    const flexEntryClass entryClass =
        flexEntryClassify(encoding, &fixedPayload);

    /* 'encodingSize' is one side of the entry's mirrored encoding; the
     * payload is everything between the forward copy and the trailer. */
    varintWidth encodingSize = 0;
    uint64_t payload_len = 0;

    switch (entryClass) {
    case FLEX_ENTRY_CLASS_IMMEDIATE:
        /* Single-byte entry: no length varint, no trailer. */
        *out_entry_len = 1;
        return true;

    case FLEX_ENTRY_CLASS_STRING: {
        /* Byte 0 starts the length varint, and the trailer mirrors the
         * whole varint for reverse traversal. */
        if (!flexSplitVarintTryDecode(fe, remaining, &encodingSize,
                                      &payload_len)) {
            return false;
        }
        break;
    }

    case FLEX_ENTRY_CLASS_CONTAINER: {
        /* Byte 0 is the container tag, mirrored by a one-byte trailer.
         * The data segment starts at fe + 1 with its own split varint,
         * and that varint's bytes count as payload. */
        varintWidth innerWidth = 0;
        uint64_t innerLen = 0;
        /* Bytes after the tag; zero makes the decode refuse. */
        if (!flexSplitVarintTryDecode(fe + 1, remaining - 1, &innerWidth,
                                      &innerLen)) {
            return false;
        }
        if (innerLen > (UINT64_MAX - (uint64_t)innerWidth)) {
            return false;
        }
        encodingSize = VARINT_WIDTH_8B; /* one side of the encoding */
        payload_len = innerLen + (uint64_t)innerWidth;
        break;
    }

    case FLEX_ENTRY_CLASS_FIXED:
        encodingSize = VARINT_WIDTH_8B; /* one side of the encoding */
        payload_len = (uint64_t)fixedPayload;
        break;

    case FLEX_ENTRY_CLASS_UNKNOWN:
    default:
        /* No writer emits this byte and no table can size it. */
        return false;
    }

    /* Compute total bytes consumed by this entry (meta + payload).  The
     * meta-size helper reads byte 0 only and returns either 1 (immediate;
     * returned above) or encodingSize * 2 (mirrored forward + reverse
     * encoding). */
    const size_t meta = FLEX_ENCODING_SIZE_TOTAL_FORWARD(fe, encodingSize);
    if (meta == 0) {
        return false;
    }
    /* Defensive overflow check on (meta + payload_len), in 64 bits:
     * payload_len is whatever the decoded varint said. */
    if (payload_len > (UINT64_MAX - (uint64_t)meta)) {
        return false;
    }
    const uint64_t total = payload_len + (uint64_t)meta;
    if (total == 0 || total > (uint64_t)remaining) {
        return false;
    }
    *out_entry_len = (size_t)total;
    return true;
}

bool flexValidateBufferDeep(const uint8_t *buf, size_t buflen) {
    /* Step 1: O(1) envelope check — leading total-bytes varint widths,
     * header room for count.  If this fails the deep walk would too. */
    if (!flexValidateBuffer(buf, buflen)) {
        return false;
    }

    /* Step 2: validate the count varint's byte-width fits within the
     * header.  flexValidateBuffer already guarantees buflen >=
     * total_bytes_width + 1 (room for at least one count byte).  Now
     * make sure the tagged varint's encoded width fits inside what
     * remains after the total-bytes varint. */
    const size_t total_bytes_width = varintSplitFullNoZeroGetLenQuick_(buf);
    const uint8_t *const count_start = buf + total_bytes_width;
    const size_t count_byte_budget = buflen - total_bytes_width;
    const size_t count_width = varintTaggedGetLenQuick_(count_start);
    if (count_width == 0 || count_width > count_byte_budget) {
        return false;
    }
    const uint64_t claimed_count = varintTaggedGet64Quick_(count_start);
    const size_t header_size = total_bytes_width + count_width;

    /* The header itself must fit inside buflen. */
    if (header_size > buflen) {
        return false;
    }

    /* Step 3: walk each entry, bound-checking every byte read.  After
     * walking `claimed_count` entries, the cursor must land exactly at
     * the buffer end (no leftover bytes, no truncation). */
    const flex *const f = (const flex *)buf;
    const uint8_t *cursor = (const uint8_t *)flexHead(f);
    const uint8_t *const end = buf + buflen;
    /* Sanity: flexHead returns buf + header_size, but FLEX_HEADER_SIZE
     * re-derives the widths.  If those derivations diverge from what we
     * computed above the buffer is malformed; refuse rather than walk
     * an inconsistent header. */
    if (cursor != buf + header_size) {
        return false;
    }

    for (uint64_t i = 0; i < claimed_count; i++) {
        if (cursor >= end) {
            return false;
        }
        /* An MDSC entry's payload is a pointer into the process that wrote
         * it.  Decoding one dereferences that pointer, so bytes from
         * outside this process must never carry one. */
        if (cursor[0] == FLEX_EXTERNAL_MDSC_48B ||
            cursor[0] == FLEX_EXTERNAL_MDSC_64B) {
            return false;
        }

        const size_t remaining = (size_t)(end - cursor);
        size_t entry_len = 0;
        if (!flexRawEntryLengthSafe((const flexEntry *)cursor, remaining,
                                    &entry_len)) {
            return false;
        }

        /* The entry fits, so its whole payload is readable. */
        if (flexNeg128OutOfRange_((const flexEntry *)cursor)) {
            return false;
        }
        cursor += entry_len;
    }

    /* The walk must terminate exactly at the buffer's claimed end — no
     * trailing slack (would mean the buffer carries extra bytes the
     * iterators won't read but a flex consumer would treat as part of
     * the structure) and no overrun (would mean entries spilled past
     * the claimed length, which the per-entry bound check above already
     * blocks). */
    if (cursor != end) {
        return false;
    }

    return true;
}

/* ====================================================================
 * compressed flex storage
 * ==================================================================== */
/* cflex layout:
 *
 * <bytes><count><compressedBytes><compressedData>
 *
 * bytes: length of data when uncompressed
 * count: count of entries inside compressedData
 * compressedBytes: length of compressedData */
#include "../deps/lz4/lz4.h"

#define CFLEX_MINIMUM_COMPRESS_BYTES 64

size_t cflexBytesCompressed(const cflex *const c) {
    varintWidth size;
    size_t l;
    varintSplitFullNoZeroGet_(c + FLEX_HEADER_SIZE(c), size, l);
    return l;
}

size_t cflexBytes(const cflex *const c) {
    const size_t headerSize = FLEX_HEADER_SIZE(c);

    varintWidth compressedBytesDescSize;
    size_t compressedBytesLen;
    varintSplitFullNoZeroGet_(c + headerSize, compressedBytesDescSize,
                              compressedBytesLen);

    return headerSize + compressedBytesDescSize + compressedBytesLen;
}

cflex *cflexDuplicate(const cflex *const c) {
    const size_t copyBytes = cflexBytes(c);
    /* @own: caller; free=zfree */
    cflex *const storage = zmalloc(copyBytes);
    memcpy(storage, c, copyBytes);
    return storage;
}

/* returns true if 'f' converted to cflex in buffer.
 * returns false if compression failed. */
/* Note: compression failure can also be because 'cBufferLen' is too
 *       small for the compressed result.
 *       We make no attempt to grow 'cBuffer' here if it isn't big enough.
 */
bool flexConvertToCFlex(const flex *f, cflex *cBuffer, size_t cBufferLen) {
    /* Discover current flex metadata. */
    const size_t totalBytes = flexTotalBytes_(f);
    const varintWidth bytesWidth = FLEX_TOTAL_BYTES_WIDTH(f);
    const varintWidth countWidth = FLEX_COUNT_WIDTH(f);

    const uint_fast32_t headerWidth = bytesWidth + countWidth;

    const void *dataWithoutHeader = f + headerWidth;
    const size_t sizeWithoutHeader = totalBytes - headerWidth;

    /* If we don't have enough data to warrant a compression run,
     * refuse to attempt compression.
     * Or, if the input cBuffer is too small,
     * also refuse compression.
     * Or, if we have *too much* data for a single LZ4 run,
     * also refuse compression. */
    /* Note: we _could_ compress more than LZ4_MAX_INPUT_SIZE by using
     *       the LZ4 streaming/framing interface, but here we expect
     * totalBytes
     *       to typically be between just 1 KB and 4 MB of data. */
    if (totalBytes < CFLEX_MINIMUM_COMPRESS_BYTES ||
        cBufferLen < CFLEX_MINIMUM_COMPRESS_BYTES ||
        totalBytes > LZ4_MAX_INPUT_SIZE) {
        return false;
    }

    /* Copy flex header to cflex */
    memcpy(cBuffer, f, headerWidth);

    static const uint_fast8_t EXPECT_LENGTH_BYTES = 2;
    /* +2 below to (optimistically) prepare for writing compressed length
     * in those two empty bytes.
     * If the compressed length requires > 2 bytes, we'll need to memmove
     * the
     * compressed data down one or more bytes. */
    /* 2 bytes of a splitFullNoZero varint stores values [65, 16447].
     * If the length of < 64 or > 16447, we'll need to memmove after
     * the entires are decompressed. */
    uint8_t *cflexStartCompressPosition =
        cBuffer + headerWidth + EXPECT_LENGTH_BYTES;
    const size_t cBufferRemainingLength =
        cBufferLen - headerWidth - EXPECT_LENGTH_BYTES;

    /* Run compression */
    int compressedLen =
        LZ4_compress_fast(dataWithoutHeader, (char *)cflexStartCompressPosition,
                          sizeWithoutHeader, cBufferRemainingLength, 1);

    /* If compress worked, populate proper encoded length of cflex.
     * else, compress failed, so return failure. */
    if (compressedLen > 0) {
        /* Write third length header to prepare the cflex for compressed
         * data.
         * Note: because we guarantee minimum buffer lengths above, we know
         *       'buffer + headerWidth + 8' will exist. */

        /* Get number of bytes required to encoded 'compressedLen' */
        varintWidth encodedLen;
        /* TODO: we have an internal one of these... try it with
         *       performance comparisons */
        varintSplitFullNoZeroLength_(encodedLen, compressedLen);

        /* If 'encodedLen' *isn't* two bytes, we need to move all our
         * compressed data either up or down a few bytes. */
        if (encodedLen > EXPECT_LENGTH_BYTES) {
            /* encoded length needs more room than we left in our writeable
             * gap, so open up more room to write full compressed length. */
            /* LZ4 only bounded the payload against the 2-byte-gap
             * budget; shifting it forward by the extra length bytes
             * needs strictly more room, so refuse rather than write
             * past cBuffer. */
            if (headerWidth + (size_t)encodedLen + (size_t)compressedLen >
                cBufferLen) {
                return false;
            }

            memmove(cflexStartCompressPosition - EXPECT_LENGTH_BYTES +
                        encodedLen,
                    cflexStartCompressPosition, compressedLen);
        } else if (encodedLen == 1) {
            /* else, we shrunk down to 64 bytes or less and can store the
             * length in just one byte, so move all compressed data up to
             * cover the one byte gap left by our original two byte
             * estimate. */
            memmove(cflexStartCompressPosition - 1, cflexStartCompressPosition,
                    compressedLen);
        } /* else, encodedLen == EXPECT_LENGTH_BYTES == 2 */

        /* now write compressed length into the correct byte position */
        varintSplitFullNoZeroPut_(cBuffer + headerWidth, encodedLen,
                                  compressedLen);

        return true;
    }

    return false;
}

static bool cflexDecompressEntriesIntoBuffer(const cflex *c, void *buffer,
                                             const size_t bufferLen) {
    /* Discover current metadata. */
    const varintWidth bytesWidth = FLEX_TOTAL_BYTES_WIDTH(c);
    const varintWidth countWidth = FLEX_COUNT_WIDTH(c);

    const uint_fast32_t headerWidth = bytesWidth + countWidth;

    /* Discover cflex compressed bytes length */
    /* Note: the next three lines are also the body of
     * cflexBytesCompressed(),
     *       but here we use the intermediate values again here (and we can
     *       use our pre-calculated values too), so we run this
     *       extraction manually. */
    varintWidth compressedBytesWidth;
    size_t totalCompressedBytes;
    varintSplitFullNoZeroGet_(c + headerWidth, compressedBytesWidth,
                              totalCompressedBytes);

    const uint_fast32_t totalHeaderWidth = headerWidth + compressedBytesWidth;
    const void *compressedDataStart = c + totalHeaderWidth;

/* decompress into buffer */
#if 1
    const int decompressedStatus = LZ4_decompress_safe(
        compressedDataStart, (char *)buffer, totalCompressedBytes, bufferLen);
#else
    (void)bufferLen;
    const size_t totalBytes = flexTotalBytes_(c);
    const size_t sizeWithoutHeader = totalBytes - headerWidth;
    const int decompressedStatus = LZ4_decompress_fast(
        compressedDataStart, (char *)buffer, sizeWithoutHeader);
#endif

    /* return true if decompress restored all flex entry bytes */
    return decompressedStatus >= 0;
}

/* returns true if 'c' is expanded to a flex in 'fBuffer'
 * returns false if decompress failed for any reason.
 * Note: if 'fBuffer' is too small to restore 'c', then we *do* realloc
 *       'fBuffer' so it can fit the expanded 'f' */
bool cflexConvertToFlex(const cflex *c, flex **fBuffer, size_t *fBufferLen) {
    const size_t totalSize = flexTotalBytes_((flex *)c);
    const uint_fast32_t headerWidth = FLEX_HEADER_SIZE(c);

    /* Check if provided buffer can fit expanded contents */
    if (*fBufferLen < totalSize) {
        /* Need to create bigger output buffer */
        size_t newSize = jebufSizeAllocation(totalSize);
        zreallocSelf(*fBuffer, newSize);
        *fBufferLen = newSize;
    }

    /* Copy cflex header to new flex */
    memcpy(*fBuffer, c, headerWidth);

    /* Decompress entries stored in 'c' into 'f' starting at the
     * proper entry offset so the flex will be fully restored
     * and ready for use after decompression. */
    return cflexDecompressEntriesIntoBuffer(c, *fBuffer + headerWidth,
                                            totalSize - headerWidth);
}

#ifdef DATAKIT_TEST

#include "strDoubleFormat.h"
#include <inttypes.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* Define KEYGEN/VALGEN before any ctest.h includes (including via list.c) */
#define CTEST_INCLUDE_KEYGEN
#define CTEST_INCLUDE_VALGEN
#include "ctest.h"

/* Note: including list.c source directly since we don't
 *       want to link it into the whole library. */
#include "list.c"
#include "list.h"

/* mds is used by testing as well... */
#include "mdsc.h"

#include "timeUtil.h" /* we time some things... */

/* Old flexGet() is used for testing and we haven't converted all tests to
 * flexGetByType() yet. */
static bool flexGet(flexEntry *fe, uint8_t **str, uint32_t *len, int64_t *val) {
    if (fe == NULL) {
        return false;
    }

    if (str) {
        *str = NULL;
    }

    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        if (str) {
            *len = entry.len;
            *str = FLEX_ENTRY_DATA(&entry);
        }
    } else {
        if (val) {
            *val = flexLoadSigned(&entry);
        }
    }
    return true;
}

static void printReadable(const void *data, const size_t len) {
    const uint8_t *fe = data;
    for (size_t j = 0; j < len; j++) {
        /* If printable character, print */
        if (fe[j] >= 32 && fe[j] <= 126) {
            fwrite(&fe[j], 1, 1, stdout);
        } else {
            /* else, print as hex */
            printf("\\x%02X", fe[j]);
        }
    }
}

void flexRepr(const flex *f) {
    size_t accumulatedSize = FLEX_TOTAL_BYTES_WIDTH(f) + FLEX_COUNT_WIDTH(f);

    printf("{total bytes %zu} {count %zu}\n"
           "{header {bytes size %d} {count size %d} %d}\n"
           "{tail offset %zu}\n",
           flexTotalBytes_(f), flexCount(f), FLEX_TOTAL_BYTES_WIDTH(f),
           FLEX_COUNT_WIDTH(f), FLEX_HEADER_SIZE(f), flexTailOffset_(f));

    /* If we have elements, the tail isn't the end.
     * If we don't have elements, the tail is the end (is also the head) */
    assert(flexCount(f) ? flexTailOffset_(f) < flexTotalBytes_(f)
                        : flexTailOffset_(f) == flexTotalBytes_(f));

    size_t index = 0;
    const flexEntry *fe = FLEX_ENTRY_HEAD(f);
    const flexEntry *const end = GET_END(f);
    const size_t howMany = flexCount(f);

    size_t i;
    for (i = 0; fe != end; i++) {
        assert(i < flexCount(f));

        flexEntryData entry = {0};
        flexEntryDataPopulate(fe, &entry);

        printf("{"
               "%p, "
               "index %3zu (%3zu), "
               "offset %5" PRIdPTR ", "
               "len %3zu, "
               "meta %2u, "
               "data %3zu"
               "} ",
               (void *)fe, index, (howMany - index), (ptrdiff_t)(fe - f),
               FLEX_ENTRY_SIZE_TOTAL(&entry), FLEX_ENTRY_META_SIZE(&entry),
               entry.len);

        accumulatedSize += FLEX_ENTRY_SIZE_TOTAL(&entry);

        if (FLEX_IS_IMMEDIATE(entry.encoding)) {
            printf("{{");
            switch (entry.encoding) {
            case FLEX_BYTES_EMPTY:
                printf("EMPTY");
                break;
            case FLEX_TRUE:
                printf("TRUE");
                break;
            case FLEX_FALSE:
                printf("FALSE");
                break;
            case FLEX_NULL:
                printf("NULL");
                break;
            default:
                __builtin_unreachable();
            }

            printf("}}");

            /* jump over single encoding byte */
            fe += entry.encodingSize;
        } else {
            /* jump over forward encoding bytes */
            fe += entry.encodingSize;

            if (FLEX_IS_STR(entry.encoding)) {
                const uint_fast8_t maxLen = 40;
                if (entry.len > maxLen) {
                    printf("%.*s...", maxLen, fe);
                } else {
#if 0
                    /* This isn't printf because printf sometimes
                     * calls strnlen() even *with* a length specifier,
                     * and it causes unnecessary "uninitialized bytes"
                     * warnings in valgrind. */
                    fwrite(fe, entry.len, 1, stdout);
#else
                    printReadable(fe, entry.len);
#endif
                }
            } else if (flexEntryIsIntegerNarrow_(entry.fe) &&
                       entry.encoding < FLEX_UINT_64B) {
                printf("[#:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (flexEntryIsIntegerNarrow_(entry.fe) &&
                       entry.encoding == FLEX_UINT_64B) {
                printf("[#:%" PRIu64 "]", flexLoadUnsigned(&entry));
            } else if (flexEntryIsIntegerWide_(entry.fe)) {
                uint8_t buf[64];
                int wroteLen;
                if (entry.encoding < FLEX_UINT_128B) {
                    wroteLen = StrInt128ToBuf(buf, sizeof(buf),
                                              flexLoadSignedBig(&entry));
                } else { /* entry.encoding == FLEX_UINT_128B */
                    wroteLen = StrUInt128ToBuf(buf, sizeof(buf),
                                               flexLoadUnsignedBig(&entry));
                }

                printf("[#:%.*s]", wroteLen, buf);
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_8) {
                printf("[REF8:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_16) {
                printf("[REF16:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_24) {
                printf("[REF24:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_32) {
                printf("[REF32:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_40) {
                printf("[REF40:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_48) {
                printf("[REF48:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_56) {
                printf("[REF56:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_CONTAINER_REFERENCE_EXTERNAL_64) {
                printf("[REF64:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (entry.encoding == FLEX_REAL_B16B) {
                printf("R16_B:%f", flexLoadFloat(&entry));
            } else if (entry.encoding == FLEX_REAL_16B) {
                printf("R16:%f", flexLoadFloat(&entry));
            } else if (entry.encoding == FLEX_REAL_32B) {
                printf("R32:%f", flexLoadFloat(&entry));
            } else if (entry.encoding == FLEX_REAL_64B) {
                printf("R64:%f", flexLoadDouble(&entry));
#if FLEX_ENABLE_PTR_MDSC
            } else if (entry.encoding == FLEX_EXTERNAL_MDSC_48B) {
                union {
                    mdsc *thing;
                    uintptr_t src;
                } t = {.src = varintExternalGet(entry.fe, 6)};
                printf("MDSC48:%.*s", (int)mdsclen(t.thing), t.thing);
            } else if (entry.encoding == FLEX_EXTERNAL_MDSC_64B) {
                union {
                    mdsc *thing;
                    uintptr_t src;
                } t = {.src = varintExternalGet(entry.fe, 8)};
                printf("MDSC64:%.*s", (int)mdsclen(t.thing), t.thing);
#endif
            }

            /* jump over data and jump over prev encoding */
            fe += entry.len + entry.encodingSize;
        }

        printf("\n");
        index++;
    }

    assert(accumulatedSize == flexTotalBytes_(f));
    assert(i == flexCount(f));

    printf("{end}\n");

#if 0
    printf("Serialized:\n");
    printReadable(f, flexBytes(f));
    printf("\n");
#endif

    printf("\n");
}

/* Allocate a fresh flex with `bytes` of over-allocated growth room so
 * timing loops measure inserts, not allocator growth. */
static flex *createFlexWithGrowthHint(size_t bytes) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    /* @own: caller; free=flexFree */
    return zrealloc(f, bytes);
}

static flex *createList() {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    flexPushBytes(&f, "foo", 3, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "quux", 4, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_HEAD);
    flexPushBytes(&f, "1024", 4, FLEX_ENDPOINT_TAIL);
    return f;
}

static flex *createIntList() {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    char buf[32] = {0};

    snprintf(buf, sizeof(buf), "100");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_TAIL);
    snprintf(buf, sizeof(buf), "128000");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_TAIL);
    snprintf(buf, sizeof(buf), "-100");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_HEAD);
    snprintf(buf, sizeof(buf), "4294967296");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_HEAD);
    snprintf(buf, sizeof(buf), "non integer");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_TAIL);
    snprintf(buf, sizeof(buf), "much much longer non integer");
    flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_TAIL);
    return f;
}

static uint64_t stress(flexEndpoint pos, int32_t num, int32_t maxsize,
                       int32_t dnum) {
    static const char location[2][5] = {"TAIL", "HEAD"};
    uint64_t total = 0;
    for (int32_t i = 0; i < maxsize; i += dnum) {
        cleaner(flexFree) flex *f = flexNew();
        for (int32_t j = 0; j < i; j++) {
            flexPushBytes(&f, "quux", 4, FLEX_ENDPOINT_TAIL);
        }

        /* Do num times a push+pop from pos */
        int64_t start = timeUtilUs();
        for (int32_t k = 0; k < num; k++) {
            /* Push to tail, delete from head. */
            flexPushBytes(&f, "quux", 4, pos);
            flexDeleteHead(&f);
        }

        int64_t end = timeUtilUs();

        printf("Entry count: %8d, bytes: %8zu, %dx push+pop (%s): %6" PRId64
               " usec\n",
               i, flexTotalBytes_(f), num, location[pos + 1], end - start);
        total += end - start;
    }

    return total;
}

static uint64_t stressReplaceInline(flexEndpoint pos, int32_t num,
                                    int32_t maxsize, int32_t dnum) {
    static const char location[2][5] = {"TAIL", "HEAD"};
    uint64_t total = 0;
    for (int32_t i = 0; i < maxsize; i += dnum) {
        cleaner(flexFree) flex *f = flexNew();
        for (int32_t j = 0; j < i; j++) {
            flexPushBytes(&f, "quux", 4, FLEX_ENDPOINT_TAIL);
        }

        /* Do num times a push+pop from pos */
        int64_t start = timeUtilUs();
        for (int32_t k = 0; k < num; k++) {
            flexReplaceBytes(&f, flexHeadOrTail(f, pos), "quux", 4);
        }

        int64_t end = timeUtilUs();

        printf("Entry count: %8d, bytes: %8zu, %dx push+pop (%s): %6" PRId64
               " usec\n",
               i, flexTotalBytes_(f), num, location[pos + 1], end - start);
        total += end - start;
    }

    return total;
}

static void pop(flex **const ff, flexEndpoint where) {
    flexEntry *fe, *vstr;
    uint32_t vlen = 8888;
    int64_t vlong = -123456789;

    fe = flexHeadOrTail(*ff, where);
    if (flexGet(fe, &vstr, &vlen, &vlong)) {
        if (where == FLEX_ENDPOINT_HEAD) {
            printf("Pop head: ");
        } else {
            printf("Pop tail: ");
        }

        if (vstr) {
            if (vlen) {
                fwrite(vstr, vlen, 1, stdout);
            }
        } else {
            printf("%" PRIi64 "", vlong);
        }

        printf("\n");
        flexDelete(ff, &fe);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

#include <fcntl.h>
DK_STATIC size_t randbytes(uint8_t *buf, size_t minlen, size_t maxlen) {
    size_t total = 0;
    size_t chunkSize = 1024; /* max 1 KB random data */

    /* guard against bad math of max="multiple * i" when i == 0 */
    if (maxlen < minlen) {
        maxlen = minlen;
    }

    size_t len = minlen + rand() % (maxlen - minlen + 1);

    /* If len < chunkSize, get len.
     * else, get one chunkSize and repeat it until len is reached. */

    size_t remaining = len;
    size_t attempt = remaining;
    if (len > chunkSize) {
        attempt = chunkSize;
    }

    int randfd = open("/dev/urandom", O_RDONLY);
    ssize_t got = read(randfd, buf, attempt);
    if (got < 0) {
        perror("read error");
        assert(NULL);
    }

    close(randfd);
    total += got;

    /* Now we just copy our 64 MB random data N times until
     * we fill our requested total buffer size.
     * If (total-len) > chunkSize, copy chunk size_t
     * else, copy (total-len) */
    while (total != len) {
        remaining = len - total;
        attempt = remaining > chunkSize ? chunkSize : remaining;
        memcpy(buf + total, buf, attempt);
        total += attempt;
    }

    assert(total == len);
    return len;
}

static int32_t randstring(uint8_t *target, uint32_t min, uint32_t max) {
    int32_t i = 0;
    int32_t len = min + rand() % (max - min + 1);
    int32_t minval = 0;
    int32_t maxval = 0;
    switch (rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
        break;
    case 1:
        minval = 48;
        maxval = 122;
        break;
    case 2:
        minval = 48;
        maxval = 52;
        break;
    default:
        assert(NULL);
    }

    while (i < len) {
        target[i++] = minval + rand() % (maxval - minval + 1);
    }

    return len;
}

/* Delete while iterating, from a flex past the 16 KiB header boundary down
 * to empty, rotating through the four cursor-keeping deletes.  After each
 * delete the cursor must sit on the entry that followed the deleted ones
 * (or at the end), and every remaining entry must hold its value. */
static int32_t flexTestDeleteCursor(const size_t payloadLen,
                                    const size_t entries) {
    int32_t bad = 0;
    char buf[64];
    assert(payloadLen >= 5 && payloadLen <= sizeof(buf));

    cleaner(zfree) void *oracleMem = zcalloc(entries, sizeof(uint32_t));
    uint32_t *const oracle = oracleMem;
    cleaner(flexFree) flex *f = flexNew();
    for (size_t i = 0; i < entries; i++) {
        oracle[i] = (uint32_t)i;
        memset(buf, 'x', payloadLen);
        memcpy(buf, &oracle[i], sizeof(oracle[i]));
        flexPushBytes(&f, buf, payloadLen, FLEX_ENDPOINT_TAIL);
    }

    size_t count = entries;
    size_t at = 0;
    size_t shrinks = 0;
    flexEntry *fe = flexHead(f);
    for (size_t step = 0; count > 0 && bad < 8; step++) {
        if (step % 3 == 0) {
            /* keep this one */
            fe = flexNext(f, fe);
            at++;
        } else {
            const uint32_t n = (step % 4 == 1 && at + 1 < count) ? 2 : 1;
            const size_t headerBefore = FLEX_HEADER_SIZE(f);
            switch (step % 4) {
            case 0:
                flexDelete(&f, &fe);
                break;
            case 1:
                flexDeleteCount(&f, &fe, n);
                break;
            case 2:
                flexDeleteDrain(&f, &fe);
                break;
            default:
                flexDeleteCountDrain(&f, &fe, 1);
                break;
            }

            memmove(&oracle[at], &oracle[at + n],
                    (count - at - n) * sizeof(*oracle));
            count -= n;
            shrinks += FLEX_HEADER_SIZE(f) < headerBefore;

            const flexEntry *want =
                at < count ? flexIndex(f, (int32_t)at) : FLEX_ENTRY_END(f);
            if (fe != want || flexCount(f) != count ||
                !flexValidateBufferDeep(f, flexBytes(f))) {
                printf("  FAIL: cursor off after a delete at %zu of %zu "
                       "(%zu-byte entries)\n",
                       at, count, payloadLen);
                bad++;
                break;
            }

            size_t i = 0;
            for (flexEntry *e = flexHeadIfAny(f); e; e = flexNext(f, e), i++) {
                databox box = {{0}};
                flexGetByType(e, &box);
                uint32_t id = UINT32_MAX;
                if (box.type == DATABOX_BYTES && box.len == payloadLen) {
                    memcpy(&id, box.data.bytes.start, sizeof(id));
                }

                if (i >= count || id != oracle[i]) {
                    printf("  FAIL: entry %zu wrong after a delete\n", i);
                    bad++;
                    break;
                }
            }
        }

        if (count > 0 && (at >= count || fe == FLEX_ENTRY_END(f))) {
            fe = flexHead(f);
            at = 0;
        }
    }

    /* The header must actually have shrunk during the walk: once below
     * 64 bytes, and again below 16 KiB for the 62-byte entries. */
    if (shrinks < (payloadLen > 5 ? 2u : 1u)) {
        printf("  FAIL: %zu header shrinks with %zu-byte entries\n", shrinks,
               payloadLen);
        bad++;
    }

    return bad;
}

static void verify(flex *f, flexEntryData *e) {
    int32_t len = flexCount(f);
    flexEntryData _e = {0};

    for (int32_t i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(flexEntryData));
        flexEntryDataPopulate(flexIndex(f, i), &e[i]);

        memset(&_e, 0, sizeof(flexEntryData));
        flexEntryDataPopulate(flexIndex(f, -len + i), &_e);

        assert(memcmp(&e[i], &_e, sizeof(flexEntryData)) == 0);
    }
}

/* 96/128-bit oracle: a value's two's-complement bits plus its sign, so one
 * array holds the whole signed and unsigned 128-bit range. */
typedef struct flexTestWide {
    __uint128_t bits;
    bool negative;
} flexTestWide;

static flexTestWide flexTestWideS(const __int128_t v) {
    return (flexTestWide){.bits = (__uint128_t)v, .negative = v < 0};
}

static flexTestWide flexTestWideU(const __uint128_t v) {
    return (flexTestWide){.bits = v, .negative = false};
}

static bool flexTestWideIsWide(const flexTestWide v) {
    return v.negative ? (__int128_t)v.bits < INT64_MIN : v.bits > UINT64_MAX;
}

/* The canonical read-back type: a non-negative value is unsigned, and a
 * value that fits 64 bits is a 64-bit type. */
static databoxType flexTestWideType(const flexTestWide v) {
    if (v.negative) {
        return flexTestWideIsWide(v) ? DATABOX_SIGNED_128 : DATABOX_SIGNED_64;
    }

    return flexTestWideIsWide(v) ? DATABOX_UNSIGNED_128 : DATABOX_UNSIGNED_64;
}

/* Box 'v' for writing.  'viaSigned' boxes a non-negative value as a
 * signed type where it fits one, to check it still reads back unsigned. */
static void flexTestWideBox(const flexTestWide v, bool viaSigned,
                            databoxBig *box) {
    const bool wide = flexTestWideIsWide(v);
    viaSigned =
        v.negative || (viaSigned && (wide ? v.bits <= (__uint128_t)INT128_MAX
                                          : v.bits <= INT64_MAX));
    memset(box, 0, sizeof(*box));
    if (wide && viaSigned) {
        DATABOX_BIG_SIGNED_128(box, (__int128_t)v.bits);
    } else if (wide) {
        DATABOX_BIG_UNSIGNED_128(box, v.bits);
    } else if (viaSigned) {
        box->type = DATABOX_SIGNED_64;
        box->data.i64 = (int64_t)(__int128_t)v.bits;
    } else {
        box->type = DATABOX_UNSIGNED_64;
        box->data.u64 = (uint64_t)v.bits;
    }
}

static bool flexTestWideMatches(const databox *got, const flexTestWide want) {
    if (got->type != flexTestWideType(want)) {
        return false;
    }

    __uint128_t bits = 0;
    switch (got->type) {
    case DATABOX_SIGNED_64:
        bits = (__uint128_t)(__int128_t)got->data.i64;
        break;
    case DATABOX_UNSIGNED_64:
        bits = got->data.u64;
        break;
    default:
        if (!got->data.ptr) {
            return false;
        }

        memcpy(&bits, got->data.ptr, sizeof(bits));
        break;
    }

    return bits == want.bits;
}

/* The value set every wide test includes. */
static size_t flexTestWideFixedSet(flexTestWide *out) {
    size_t n = 0;
    out[n++] = flexTestWideU((__uint128_t)1 << 64);
    out[n++] = flexTestWideU(((__uint128_t)1 << 95) - 1);
    out[n++] = flexTestWideU((__uint128_t)1 << 95);
    out[n++] = flexTestWideU((__uint128_t)1 << 127);
    out[n++] = flexTestWideU(UINT128_MAX);
    out[n++] = flexTestWideS(-((__int128_t)1 << 63) - 1);
    out[n++] = flexTestWideS(-((__int128_t)1 << 95));
    out[n++] = flexTestWideS(INT128_MIN);
    out[n++] = flexTestWideS(INT128_MAX);
    return n;
}

/* 2^s - 1, 2^s, -2^s and -2^s - 1 for every s < 128, then the fixed set,
 * which together hit every integer encoding width. */
#define FLEX_TEST_WIDE_SWEEP_MAX (128 * 4 + 16)
static size_t flexTestWideSweep(flexTestWide *out) {
    size_t n = 0;
    for (uint32_t s = 0; s < 128; s++) {
        const __uint128_t p = (__uint128_t)1 << s;
        out[n++] = flexTestWideU(p - 1);
        out[n++] = flexTestWideU(p);
        if (s < 127) {
            out[n++] = flexTestWideS(-(__int128_t)p);
            out[n++] = flexTestWideS(-(__int128_t)p - 1);
        } else {
            out[n++] = flexTestWideS(INT128_MIN);
        }
    }

    return n + flexTestWideFixedSet(out + n);
}

static int32_t flexTestWideDecoders(void) {
    int32_t bad = 0;
    flexTestWide vals[FLEX_TEST_WIDE_SWEEP_MAX];
    const size_t n = flexTestWideSweep(vals);

    cleaner(flexFree) flex *f = flexNew();
    for (size_t i = 0; i < n; i++) {
        databoxBig box;
        flexTestWideBox(vals[i], i % 2 == 1, &box);
        flexPushDataboxBig(&f, &box, FLEX_ENDPOINT_TAIL);
    }

    bool seen[256] = {0};
    for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
        seen[fe[0]] = true;
    }

    for (uint32_t e = FLEX_NEG_8B; e <= FLEX_UINT_128B; e++) {
        if (!seen[e]) {
            printf("  FAIL: no entry with integer encoding %" PRIu32 "\n", e);
            bad++;
        }
    }

    /* flexGetByTypeBig ignores a poisoned destination entirely. */
    size_t i = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe && i < n;
         fe = flexNext(f, fe), i++) {
        databoxBig b;
        memset(&b, 0xFF, sizeof(b));
        flexGetByTypeBig(fe, &b);
        const bool wide = flexTestWideIsWide(vals[i]);
        if (!flexTestWideMatches((const databox *)&b, vals[i]) || b.allocated ||
            b.created || b.big != wide ||
            (wide && b.data.u128 != (__uint128_t *)b.extra)) {
            printf("  FAIL: flexGetByTypeBig entry %zu\n", i);
            bad++;
        }
    }

    if (i != n) {
        printf("  FAIL: decoded %zu of %zu entries\n", i, n);
        bad++;
    }

    /* flexGetByTypeInto: a wide value lands in the slot and the box borrows
     * it.  A narrow value writes only type and data, so the poisoned flag
     * bits and the poisoned slot survive it.  A NULL slot defers a wide
     * value and changes nothing for a narrow one. */
    i = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe && i < n;
         fe = flexNext(f, fe), i++) {
        const bool wide = flexTestWideIsWide(vals[i]);
        databoxWide untouched;
        memset(&untouched, 0xA5, sizeof(untouched));
        databoxWide slot = untouched;
        databox box;
        memset(&box, 0xFF, sizeof(box));
        flexGetByTypeInto(fe, &box, &slot);
        bool ok = flexTestWideMatches(&box, vals[i]);
        if (wide) {
            ok = ok && box.big && !box.allocated && box.data.ptr == &slot;
        } else {
            ok = ok && box.big && box.allocated && box.created &&
                 memcmp(&slot, &untouched, sizeof(slot)) == 0;
        }

        databox deferred;
        memset(&deferred, 0xFF, sizeof(deferred));
        flexGetByTypeInto(fe, &deferred, NULL);
        if (wide) {
            ok = ok && databoxIsWideDeferred(&deferred) &&
                 deferred.type == flexTestWideType(vals[i]) && !deferred.big &&
                 !deferred.allocated;
        } else {
            ok = ok && flexTestWideMatches(&deferred, vals[i]);
        }

        if (!ok) {
            printf("  FAIL: flexGetByTypeInto entry %zu\n", i);
            bad++;
        }
    }

    /* Narrow decodes are byte-identical through flexGetByType and
     * flexGetByTypeInto, for every narrow type. */
    {
        cleaner(flexFree) flex *mixed = flexNew();
        const databox narrow[] = {
            databoxNewSigned(-5),
            databoxNewUnsigned(77),
            DATABOX_DOUBLE(1.5),
            databoxNull(),
            databoxBool(true),
            databoxBool(false),
            databoxNewBytesString("short"),
            databoxNewBytesString("a value longer than eight bytes"),
            databoxNewBytesString(""),
        };
        for (size_t k = 0; k < COUNT_ARRAY(narrow); k++) {
            flexPushByType(&mixed, &narrow[k], FLEX_ENDPOINT_TAIL);
        }

        size_t k = 0;
        for (flexEntry *fe = flexHeadIfAny(mixed); fe;
             fe = flexNext(mixed, fe), k++) {
            databox viaGet = {{0}};
            databox viaInto = {{0}};
            databoxWide slot;
            flexGetByType(fe, &viaGet);
            flexGetByTypeInto(fe, &viaInto, &slot);
            if (memcmp(&viaGet, &viaInto, sizeof(viaGet)) != 0 ||
                databoxCompare(&viaInto, &narrow[k]) != 0) {
                printf("  FAIL: narrow Into decode %zu differs\n", k);
                bad++;
            }
        }

        if (k != COUNT_ARRAY(narrow)) {
            printf("  FAIL: mixed flex decoded %zu of %zu\n", k,
                   COUNT_ARRAY(narrow));
            bad++;
        }
    }

    /* flexGetByTypeIntoWithReference resolves a reference to a wide value
     * into the slot (deferred with a NULL slot), and one to a narrow value
     * in place.  Each atom table holds a single value. */
    {
        __uint128_t wideStore = ((__uint128_t)1 << 100) + 17;
        databox wideVal = {
            .type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &wideStore};
        const databox bytesVal =
            databoxNewBytesString("an atom value longer than eight");
        cleaner(multimapAtomFree) multimapAtom *wideAtoms = multimapAtomNew();
        cleaner(multimapAtomFree) multimapAtom *bytesAtoms = multimapAtomNew();
        databox wideRef = wideVal;
        databox bytesRef = bytesVal;
        multimapAtomInsertConvert(wideAtoms, &wideRef);
        multimapAtomInsertConvert(bytesAtoms, &bytesRef);

        cleaner(flexFree) flex *refs = flexNew();
        flexPushByType(&refs, &wideRef, FLEX_ENDPOINT_TAIL);
        flexPushByType(&refs, &bytesRef, FLEX_ENDPOINT_TAIL);
        flexEntry *const wideFE = flexHead(refs);
        flexEntry *const bytesFE = flexNext(refs, wideFE);

        databoxWide slot;
        databox got;
        memset(&got, 0xFF, sizeof(got));
        flexGetByTypeIntoWithReference(wideFE, &got, &slot, wideAtoms);
        const bool wideOk = got.type == DATABOX_UNSIGNED_128 && got.big &&
                            !got.allocated && got.data.ptr == &slot &&
                            *got.data.u128 == wideStore;

        memset(&got, 0xFF, sizeof(got));
        flexGetByTypeIntoWithReference(wideFE, &got, NULL, wideAtoms);
        const bool deferredOk = databoxIsWideDeferred(&got) &&
                                got.type == DATABOX_UNSIGNED_128 && !got.big;

        databox gotBytes = {{0}};
        flexGetByTypeIntoWithReference(bytesFE, &gotBytes, &slot, bytesAtoms);
        const bool bytesOk = databoxEqual(&gotBytes, &bytesVal);

        /* An id the table doesn't hold stays the id. */
        const databox unknownRef = {.type =
                                        DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                    .data.u = wideRef.data.u + 1000};
        flexPushByType(&refs, &unknownRef, FLEX_ENDPOINT_TAIL);
        databox miss = {{0}};
        flexGetByTypeIntoWithReference(flexTail(refs), &miss, &slot, wideAtoms);
        const bool missOk = miss.type == DATABOX_CONTAINER_REFERENCE_EXTERNAL &&
                            miss.data.u == unknownRef.data.u;

        if (!wideOk || !deferredOk || !bytesOk || !missOk) {
            printf("  FAIL: flexGetByTypeIntoWithReference (wide %d, "
                   "deferred %d, bytes %d, miss %d)\n",
                   wideOk, deferredOk, bytesOk, missOk);
            bad++;
        }
    }

    /* flexGetNextByTypeBig walks the same values. */
    {
        flexEntry *fe = flexHead(f);
        databoxBig b;
        memset(&b, 0xFF, sizeof(b));
        flexGetByTypeBig(fe, &b);
        bool ok = flexTestWideMatches((const databox *)&b, vals[0]);
        size_t walked = 1;
        memset(&b, 0xFF, sizeof(b));
        while (flexGetNextByTypeBig(f, &fe, &b)) {
            ok = ok && walked < n &&
                 flexTestWideMatches((const databox *)&b, vals[walked]) &&
                 b.big == flexTestWideIsWide(vals[walked]) && !b.allocated;
            walked++;
            memset(&b, 0xFF, sizeof(b));
        }

        if (!ok || walked != n || fe) {
            printf("  FAIL: flexGetNextByTypeBig walked %zu of %zu\n", walked,
                   n);
            bad++;
        }
    }

    /* flexGetByTypeCopy: a plain box gets a heap-owned wide value, and a
     * DATABOX_BIG_INIT'd box gets it in its own extra. */
    i = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe && i < n;
         fe = flexNext(f, fe), i++) {
        const bool wide = flexTestWideIsWide(vals[i]);
        databox plain = {{0}};
        flexGetByTypeCopy(fe, &plain);
        const uint8_t *const p = plain.data.ptr;
        const bool inFlex =
            p >= (const uint8_t *)f && p < (const uint8_t *)f + flexBytes(f);
        if (!flexTestWideMatches(&plain, vals[i]) || plain.allocated != wide ||
            plain.big != wide ||
            (wide && (inFlex || ((uintptr_t)p % 16) != 0))) {
            printf("  FAIL: flexGetByTypeCopy into a plain box, entry %zu\n",
                   i);
            bad++;
        }

        databoxFreeData(&plain);
        if (wide &&
            (plain.type != DATABOX_VOID || plain.data.ptr || plain.big)) {
            printf("  FAIL: copied wide entry %zu not released\n", i);
            bad++;
        }

        databoxBig big;
        memset(&big, 0xFF, sizeof(big));
        DATABOX_BIG_INIT(&big);
        flexGetByTypeCopy(fe, (databox *)&big);
        if (!flexTestWideMatches((const databox *)&big, vals[i]) ||
            big.big != wide ||
            (wide && big.data.u128 != (__uint128_t *)big.extra) ||
            (!wide && big.allocated)) {
            printf("  FAIL: flexGetByTypeCopy into a big box, entry %zu\n", i);
            bad++;
        }

        /* The payload is inline, so this releases nothing. */
        databoxFreeData((databox *)&big);
        if (wide && !flexTestWideMatches((const databox *)&big, vals[i])) {
            printf("  FAIL: FreeData touched inline entry %zu\n", i);
            bad++;
        }
    }

    const __uint128_t wu = UINT128_MAX - 3;
    const __int128_t ws = INT128_MIN + 3;
    databoxBig wU;
    databoxBig wS;
    DATABOX_BIG_UNSIGNED_128(&wU, wu);
    DATABOX_BIG_SIGNED_128(&wS, ws);

    /* One box reused across copies: a narrow copy drops the capability,
     * so the next wide copy goes to the heap instead of through the
     * narrow value. */
    {
        cleaner(flexFree) flex *two = flexNew();
        flexPushSigned(&two, 7, FLEX_ENDPOINT_TAIL);
        flexPushDataboxBig(&two, &wU, FLEX_ENDPOINT_TAIL);
        databoxBig reuse;
        memset(&reuse, 0, sizeof(reuse));
        DATABOX_BIG_INIT(&reuse);
        flexGetByTypeCopy(flexIndex(two, 0), (databox *)&reuse);
        const bool narrowOk = reuse.type == DATABOX_UNSIGNED_64 &&
                              reuse.data.u64 == 7 && !reuse.big;
        flexGetByTypeCopy(flexIndex(two, 1), (databox *)&reuse);
        if (!narrowOk || reuse.type != DATABOX_UNSIGNED_128 || !reuse.big ||
            !reuse.allocated || reuse.data.u128 == (__uint128_t *)reuse.extra ||
            *reuse.data.u128 != wu) {
            printf("  FAIL: reused copy box\n");
            bad++;
        }

        databoxFreeData((databox *)&reuse);
    }

    /* flexGetTwoDataboxesBig decodes every pair shape. */
    {
        cleaner(flexFree) flex *p1 = flexNew();
        flexPushDataboxBig(&p1, &wU, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&p1, "pair payload", 12, FLEX_ENDPOINT_TAIL);
        cleaner(flexFree) flex *p2 = flexNew();
        flexPushSigned(&p2, -5, FLEX_ENDPOINT_TAIL);
        flexPushDataboxBig(&p2, &wS, FLEX_ENDPOINT_TAIL);
        cleaner(flexFree) flex *p3 = flexNew();
        flexPushDataboxBig(&p3, &wS, FLEX_ENDPOINT_TAIL);
        flexPushDataboxBig(&p3, &wU, FLEX_ENDPOINT_TAIL);

        databoxBig a;
        databoxBig b;
        memset(&a, 0xFF, sizeof(a));
        memset(&b, 0xFF, sizeof(b));
        bool ok = flexGetTwoDataboxesBig(p1, flexBytes(p1), &a, &b) &&
                  a.type == DATABOX_UNSIGNED_128 && a.big && !a.allocated &&
                  a.data.u128 == (__uint128_t *)a.extra && *a.data.u128 == wu &&
                  b.type == DATABOX_BYTES && !b.big && !b.allocated &&
                  b.len == 12 &&
                  (const uint8_t *)b.data.bytes.start > (const uint8_t *)p1 &&
                  (const uint8_t *)b.data.bytes.start <
                      (const uint8_t *)p1 + flexBytes(p1) &&
                  memcmp(b.data.bytes.start, "pair payload", 12) == 0;

        memset(&a, 0xFF, sizeof(a));
        memset(&b, 0xFF, sizeof(b));
        ok = ok && flexGetTwoDataboxesBig(p2, flexBytes(p2), &a, &b) &&
             a.type == DATABOX_SIGNED_64 && a.data.i64 == -5 && !a.big &&
             !a.allocated && b.type == DATABOX_SIGNED_128 && b.big &&
             b.data.i128 == (__int128_t *)b.extra && *b.data.i128 == ws;

        memset(&a, 0xFF, sizeof(a));
        memset(&b, 0xFF, sizeof(b));
        ok = ok && flexGetTwoDataboxesBig(p3, flexBytes(p3), &a, &b) &&
             a.type == DATABOX_SIGNED_128 && *a.data.i128 == ws &&
             b.type == DATABOX_UNSIGNED_128 && *b.data.u128 == wu;
        if (!ok) {
            printf("  FAIL: flexGetTwoDataboxesBig pair decode\n");
            bad++;
        }

        /* Refusals leave both boxes untouched. */
        cleaner(flexFree) flex *one = flexNewFromDataboxBig(&wU);
        cleaner(flexFree) flex *three = flexDuplicate(p3);
        flexPushSigned(&three, 1, FLEX_ENDPOINT_TAIL);
        const struct {
            const flex *f;
            size_t len;
        } refuse[] = {
            {one, flexBytes(one)},
            {three, flexBytes(three)},
            {p3, flexBytes(p3) - 1},
        };
        for (size_t r = 0; r < COUNT_ARRAY(refuse); r++) {
            databoxBig poison;
            memset(&poison, 0xFF, sizeof(poison));
            memset(&a, 0xFF, sizeof(a));
            memset(&b, 0xFF, sizeof(b));
            if (flexGetTwoDataboxesBig(refuse[r].f, refuse[r].len, &a, &b) ||
                memcmp(&a, &poison, sizeof(a)) != 0 ||
                memcmp(&b, &poison, sizeof(b)) != 0) {
                printf("  FAIL: flexGetTwoDataboxesBig refusal %zu\n", r);
                bad++;
            }
        }
    }

    /* flexGetSingleDataboxBig decodes with the same capability rules. */
    {
        cleaner(flexFree) flex *s1 = flexNew();
        flexPushSigned(&s1, -77, FLEX_ENDPOINT_TAIL);
        databoxBig o;
        memset(&o, 0xFF, sizeof(o));
        if (!flexGetSingleDataboxBig(s1, flexBytes(s1), &o) ||
            o.type != DATABOX_SIGNED_64 || o.data.i64 != -77 || o.big ||
            o.allocated) {
            printf("  FAIL: flexGetSingleDataboxBig narrow decode\n");
            bad++;
        }
    }

    /* flexGetSigned / flexGetUnsigned refuse a value wider than 64 bits. */
    {
        cleaner(flexFree) flex *g = flexNew();
        flexPushDataboxBig(&g, &wU, FLEX_ENDPOINT_TAIL);
        flexPushDataboxBig(&g, &wS, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&g, -7, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&g, 7, FLEX_ENDPOINT_TAIL);
        int64_t sv = 1;
        uint64_t uv = 1;
        const bool ok = !flexGetSigned(flexIndex(g, 0), &sv) &&
                        !flexGetUnsigned(flexIndex(g, 0), &uv) &&
                        !flexGetSigned(flexIndex(g, 1), &sv) &&
                        !flexGetUnsigned(flexIndex(g, 1), &uv) && sv == 1 &&
                        uv == 1 && flexGetSigned(flexIndex(g, 2), &sv) &&
                        sv == -7 && flexGetUnsigned(flexIndex(g, 3), &uv) &&
                        uv == 7;
        if (!ok) {
            printf("  FAIL: flexGetSigned / flexGetUnsigned on a wide value\n");
            bad++;
        }
    }

    return bad;
}

/* ====================================================================
 * Validator mutation sweep
 * ====================================================================
 * Feeds every single-byte mutation (each offset x each other byte value)
 * of several valid flexes to flexValidateBuffer, flexValidateBufferDeep
 * and flexPeekTotalBytes.  An assertion on the validate path aborts the
 * test; the last corpus line printed names the buffer.  Checked for
 * every mutation:
 *   - the shallow gate accepts exactly when the peek succeeds and
 *     reports the whole buffer, and a peek over a longer outer buffer
 *     reports the same prefix;
 *   - every buffer the deep gate accepts decodes the way an untrusted
 *     reader would decode it: flexHead/flexNext visit exactly flexCount
 *     entries, flexGetByTypeBig returns in-bounds payloads
 *     and never a pointer, a plain databox gets the type only for a
 *     96/128-bit entry, and when every trailer is the one the writer
 *     emits, flexTail/flexPrev visit the same entries in reverse.
 * Unmutated buffers must pass both gates and have intact trailers.
 *
 * Corpus: strings (split-varint lengths of one, two and three bytes),
 * integers and immediates, 96/128-bit integers, reals, references and
 * empty bytes (the fixed-width table), nested containers (inner varints
 * of one and two bytes), and a flex whose total-bytes header varint is
 * multi-byte. */
#define FLEX_SWEEP_BYTE_VALUES 256
#define FLEX_SWEEP_PEEK_SLACK 16

/* True when the bytes after the entry at 'fe' are the trailer the
 * writer emits for that entry, so reverse traversal over it is defined.
 * 'entryLen' is the entry's size from the forward walk; reads stay
 * inside [fe, fe + entryLen). */
static bool flexValidateSweepTrailerIntact(const flexEntry *const fe,
                                           const size_t entryLen) {
    const flexEncoding encoding = fe[0];
    if (FLEX_IS_IMMEDIATE(encoding)) {
        return entryLen == 1;
    }

    uint8_t expected[16];
    varintWidth width;
    if (FLEX_IS_STR(encoding)) {
        /* [LEN][DATA][LEN REVERSED] */
        varintWidth forwardWidth;
        size_t len;
        varintSplitFullNoZeroGet_(fe, forwardWidth, len);
        varintSplitFullNoZeroReversedPutForward_(expected, width, len);
        return width == forwardWidth && entryLen == len + 2 * (size_t)width &&
               memcmp(fe + entryLen - width, expected, width) == 0;
    }

    if (fe[entryLen - 1] != encoding) {
        return false;
    }

    if (FLEX_IS_FORWARD_DECLARE_SUBCONTAINER(encoding)) {
        /* [TAG][NESTED FLEX][NESTED SIZE REVERSED][TAG] */
        varintWidth innerWidth;
        size_t innerLen;
        varintSplitFullNoZeroGet_(fe + 1, innerWidth, innerLen);
        varintSplitFullNoZeroReversedPutForward_(expected, width, innerLen);
        return width == innerWidth && entryLen == innerLen + width + 2 &&
               memcmp(fe + 1 + innerLen, expected, width) == 0;
    }

    return true;
}

/* Decode a buffer flexValidateBufferDeep accepted.  'seen' has room for
 * one entry pointer per buffer byte.  With 'requireTrailers' a trailer
 * the writer would not emit is a failure; otherwise it only skips the
 * reverse walk, which the deep gate does not cover.  Returns the number
 * of failures; '*reversed' reports whether the reverse walk ran. */
static int32_t
flexValidateSweepDecode(const char *const name, const uint8_t *const buf,
                        const size_t bytes, const size_t pos, const uint32_t v,
                        const bool requireTrailers,
                        const flexEntry **const seen, bool *const reversed) {
    const flex *const f = (const flex *)buf;
    const uint8_t *const end = buf + bytes;
    const size_t count = flexCount(f);
    size_t visited = 0;
    bool trailersIntact = true;
    *reversed = false;

#define FLEX_SWEEP_DECODE_FAIL_(what)                                          \
    do {                                                                       \
        printf("  ERROR! %s: offset %zu value %" PRIu32 ": %s\n", name, pos,   \
               v, (what));                                                     \
        return 1;                                                              \
    } while (0)

    flexEntry *fe = flexHeadIfAny(f);
    while (fe) {
        if (visited == count || fe >= end) {
            FLEX_SWEEP_DECODE_FAIL_("forward walk passed the entry count");
        }

        if (fe[0] == FLEX_EXTERNAL_MDSC_48B ||
            fe[0] == FLEX_EXTERNAL_MDSC_64B) {
            FLEX_SWEEP_DECODE_FAIL_("deep gate accepted a pointer entry");
        }

        databoxBig big;
        flexGetByTypeBig(fe, &big);

        /* Variable-length payloads must lie inside the buffer. */
        const bool variable = big.type == DATABOX_BYTES ||
                              (big.type >= DATABOX_CONTAINER_FLEX_MAP &&
                               big.type <= DATABOX_CONTAINER_FLEX_TUPLE);
        const uint8_t *const start = (const uint8_t *)big.data.bytes.start;
        if (variable && big.len &&
            (start < fe || start >= end || big.len > (size_t)(end - start))) {
            FLEX_SWEEP_DECODE_FAIL_("payload outside the buffer");
        }

        /* A plain box gets the type of a 96/128-bit entry and no write. */
        databox plain = {{0}};
        flexGetByType(fe, &plain);
        if (plain.type != big.type) {
            FLEX_SWEEP_DECODE_FAIL_("plain and big decodes disagree");
        }

        if ((plain.type == DATABOX_SIGNED_128 ||
             plain.type == DATABOX_UNSIGNED_128) &&
            plain.data.ptr) {
            FLEX_SWEEP_DECODE_FAIL_("plain box got a 128-bit pointer");
        }

        flexEntry *const next = flexNext(f, fe);
        const size_t entryLen = (size_t)((next ? next : end) - fe);
        trailersIntact =
            trailersIntact && flexValidateSweepTrailerIntact(fe, entryLen);
        seen[visited++] = fe;
        fe = next;
    }

    if (visited != count) {
        FLEX_SWEEP_DECODE_FAIL_("forward walk visited fewer entries");
    }

    if (!trailersIntact) {
        if (requireTrailers) {
            FLEX_SWEEP_DECODE_FAIL_("an entry's trailer is not the writer's");
        }

        return 0;
    }

    /* Every trailer is the writer's, so reverse traversal must mirror the
     * forward walk exactly. */
    fe = count ? flexTail(f) : NULL;
    for (size_t i = count; i-- > 0;) {
        if (fe != seen[i]) {
            FLEX_SWEEP_DECODE_FAIL_("reverse walk diverged");
        }

        databoxBig big;
        flexGetByTypeBig(fe, &big);
        fe = flexPrev(f, fe);
    }

    if (fe) {
        FLEX_SWEEP_DECODE_FAIL_("reverse walk passed the head");
    }

#undef FLEX_SWEEP_DECODE_FAIL_

    *reversed = true;
    return 0;
}

/* Sweep one valid flex.  Offsets in [skipFrom, skipTo) are not mutated
 * (pass 0, 0 to mutate every offset); a long string's interior bytes are
 * payload no validator reads.  Returns the number of failures found. */
static int32_t flexValidateTotalitySweepOne(const char *const name,
                                            const flex *const f,
                                            const size_t skipFrom,
                                            const size_t skipTo) {
    int32_t failures = 0;
    const size_t bytes = flexBytes(f);
    const size_t mutated = bytes - (skipTo - skipFrom);

    printf("  %s: %zu bytes, %zu entries, %zu mutations\n", name, bytes,
           flexCount(f), mutated * (FLEX_SWEEP_BYTE_VALUES - 1));
    fflush(stdout);

    /* Slack after the buffer lets the peek see a longer outer buffer. */
    cleanerName(zfree, Bytes) uint8_t *scratch =
        zcalloc(1, bytes + FLEX_SWEEP_PEEK_SLACK);
    memcpy(scratch, f, bytes);

    cleaner(zfree) void *seenStore = zcalloc(bytes, sizeof(flexEntry *));
    const flexEntry **const seen = seenStore;

    bool reversed = false;
    if (!flexValidateBuffer(scratch, bytes)) {
        printf("  ERROR! %s: unmutated buffer failed the shallow gate\n", name);
        return failures + 1;
    }

    if (!flexValidateBufferDeep(scratch, bytes)) {
        printf("  ERROR! %s: unmutated buffer failed the deep gate\n", name);
        return failures + 1;
    }

    failures += flexValidateSweepDecode(
        name, scratch, bytes, 0, FLEX_SWEEP_BYTE_VALUES, true, seen, &reversed);

    size_t accepted = 0;
    size_t reverseWalked = 0;
    for (size_t pos = 0; pos < bytes; pos++) {
        if (pos >= skipFrom && pos < skipTo) {
            continue;
        }

        const uint8_t honest = scratch[pos];
        for (uint32_t v = 0; v < FLEX_SWEEP_BYTE_VALUES; v++) {
            if ((uint8_t)v == honest) {
                continue;
            }

            scratch[pos] = (uint8_t)v;

            /* An abort in any call is a failure this sweep detects. */
            const bool shallow = flexValidateBuffer(scratch, bytes);
            const bool deep = flexValidateBufferDeep(scratch, bytes);

            /* The shallow gate accepts exactly the buffers whose header
             * claims the whole buffer, which is what the peek reports. */
            size_t total = 0;
            const bool peeked = flexPeekTotalBytes(scratch, bytes, &total);
            if (shallow != (peeked && total == bytes) ||
                (peeked && total > bytes)) {
                printf("  ERROR! %s: offset %zu value %" PRIu32
                       " shallow %d disagrees with peek %d (total %zu)\n",
                       name, pos, v, shallow, peeked, total);
                failures++;
            }

            /* A longer outer buffer leaves a successful peek unchanged. */
            size_t outerTotal = 0;
            const bool outerPeeked = flexPeekTotalBytes(
                scratch, bytes + FLEX_SWEEP_PEEK_SLACK, &outerTotal);
            if (peeked && (!outerPeeked || outerTotal != total)) {
                printf("  ERROR! %s: offset %zu value %" PRIu32
                       " peek changed with a longer outer buffer\n",
                       name, pos, v);
                failures++;
            }

            if (deep) {
                if (!shallow) {
                    printf("  ERROR! %s: offset %zu value %" PRIu32
                           " passed deep and failed shallow\n",
                           name, pos, v);
                    failures++;
                }

                accepted++;
                failures += flexValidateSweepDecode(name, scratch, bytes, pos,
                                                    v, false, seen, &reversed);
                reverseWalked += reversed;
            }
        }

        scratch[pos] = honest;
    }

    printf("    deep accepted %zu mutations, decoded all, reverse-walked "
           "%zu\n",
           accepted, reverseWalked);

    return failures;
}

/* Read the entry at 'index' of 'outer' (walking from the head for a
 * non-negative index, from the tail otherwise) and require it to be a
 * nested FLEX_MAP whose bytes equal 'inner'.  Returns the number of
 * failures. */
static int32_t flexValidateSweepCheckNested(const flex *const outer,
                                            const int32_t index,
                                            const flex *const inner) {
    databox got = {{0}};
    const flexEntry *const fe = flexIndexDirect(outer, index);
    if (fe) {
        flexGetByType(fe, &got);
    }

    if (got.type != DATABOX_CONTAINER_FLEX_MAP ||
        flexBytes(got.data.bytes.start) != flexBytes(inner) ||
        memcmp(got.data.bytes.start, inner, flexBytes(inner)) != 0) {
        printf("  ERROR! nested: entry at index %" PRId32
               " is not the nested map (type %d)\n",
               index, (int)got.type);
        return 1;
    }

    return 0;
}

int32_t flexValidateTotalitySweep(void) {
    int32_t failures = 0;

    printf("Test flex validators are total over every single-byte "
           "mutation:\n");

    {
        /* Strings of several lengths: the split-varint length path at
         * its 1-byte and 2-byte forms. */
        cleaner(flexFree) flex *strs = flexNew();
        static const char *const words[] = {"a", "bc", "element",
                                            "a longer member so the entry "
                                            "length varint grows past one "
                                            "byte of storage"};
        for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++) {
            flexPushBytes(&strs, words[i], strlen(words[i]),
                          FLEX_ENDPOINT_TAIL);
        }
        failures += flexValidateTotalitySweepOne("strings", strs, 0, 0);
    }

    {
        /* One string long enough for a 3-byte length varint, between
         * shorter ones.  Only its edges are mutated: the interior is
         * payload no validator reads. */
        enum { SWEEP_LONG_STRING_ = 16500, SWEEP_LONG_EDGE_ = 24 };
        cleanerName(zfree, Bytes) uint8_t *longString =
            zmalloc(SWEEP_LONG_STRING_);
        memset(longString, 'x', SWEEP_LONG_STRING_);

        cleaner(flexFree) flex *wideStrs = flexNew();
        flexPushBytes(&wideStrs, "head", strlen("head"), FLEX_ENDPOINT_TAIL);
        flexPushBytes(&wideStrs, longString, SWEEP_LONG_STRING_,
                      FLEX_ENDPOINT_TAIL);
        flexPushBytes(&wideStrs, "tail", strlen("tail"), FLEX_ENDPOINT_TAIL);

        const flexEntry *const longEntry = flexIndexDirect(wideStrs, 1);
        assert(varintSplitFullNoZeroGetLenQuick_(longEntry) == 3);
        const size_t payloadStart = (size_t)(longEntry - wideStrs) + 3;
        failures += flexValidateTotalitySweepOne(
            "3-byte-string", wideStrs, payloadStart + SWEEP_LONG_EDGE_,
            payloadStart + SWEEP_LONG_STRING_ - SWEEP_LONG_EDGE_);
    }

    {
        /* Integers across the signed/unsigned width ladder plus the
         * immediates: the fixed-width payload table. */
        cleaner(flexFree) flex *nums = flexNew();
        static const int64_t values[] = {
            0, 1, -1, INT8_MAX, INT16_MIN, INT32_MAX, INT64_MIN, INT64_MAX};
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
            flexPushSigned(&nums, values[i], FLEX_ENDPOINT_TAIL);
        }
        const databox t = {.type = DATABOX_TRUE};
        const databox fa = {.type = DATABOX_FALSE};
        const databox nil = {.type = DATABOX_NULL};
        flexPushByType(&nums, &t, FLEX_ENDPOINT_TAIL);
        flexPushByType(&nums, &fa, FLEX_ENDPOINT_TAIL);
        flexPushByType(&nums, &nil, FLEX_ENDPOINT_TAIL);
        failures += flexValidateTotalitySweepOne("numbers", nums, 0, 0);
    }

    {
        /* The rest of the fixed-width table: 96- and 128-bit integers of
         * both signs, every real width, references of several widths, and
         * empty bytes. */
        cleaner(flexFree) flex *fixed = flexNew();
        databoxBig wide;
        DATABOX_BIG_UNSIGNED_128(&wide, (__uint128_t)1 << 80);
        flexPushDataboxBig(&fixed, &wide, FLEX_ENDPOINT_TAIL);
        DATABOX_BIG_UNSIGNED_128(&wide, ((__uint128_t)1 << 100) + 7);
        flexPushDataboxBig(&fixed, &wide, FLEX_ENDPOINT_TAIL);
        DATABOX_BIG_SIGNED_128(&wide, -((__int128_t)1 << 80));
        flexPushDataboxBig(&fixed, &wide, FLEX_ENDPOINT_TAIL);
        DATABOX_BIG_SIGNED_128(&wide, -((__int128_t)1 << 100) - 7);
        flexPushDataboxBig(&fixed, &wide, FLEX_ENDPOINT_TAIL);

        flexPushFloat16(&fixed, 1.5f, FLEX_ENDPOINT_TAIL);
        flexInsertFloatB16(&fixed, FLEX_ENTRY_AFTER_TAIL(fixed), -2.0f);
        flexPushFloat(&fixed, 3.14159f, FLEX_ENDPOINT_TAIL);
        flexPushDouble(&fixed, 2.718281828459045, FLEX_ENDPOINT_TAIL);

        static const uint64_t refs[] = {
            0, 255, 256, 70000, UINT64_C(1) << 40, UINT64_MAX};
        for (size_t i = 0; i < sizeof(refs) / sizeof(*refs); i++) {
            const databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                 .data.u64 = refs[i]};
            const databox *refBox[1] = {&ref};
            flexAppendMultipleExact(&fixed, 1, refBox);
        }

        flexPushBytes(&fixed, "", 0, FLEX_ENDPOINT_TAIL);
        failures += flexValidateTotalitySweepOne("fixed-width", fixed, 0, 0);
    }

    {
        /* Nested container entries: the inner-varint path, which is a
         * second split varint inside one entry's payload.  'inner' needs
         * a one-byte inner varint, 'wide' a two-byte one. */
        cleaner(flexFree) flex *inner = flexNew();
        flexPushBytes(&inner, "inner", strlen("inner"), FLEX_ENDPOINT_TAIL);
        flexPushSigned(&inner, -12345, FLEX_ENDPOINT_TAIL);

        cleaner(flexFree) flex *wide = flexNew();
        static const char longMember[] = "a nested member long enough that "
                                         "its flex needs more than 64 bytes";
        flexPushBytes(&wide, longMember, strlen(longMember),
                      FLEX_ENDPOINT_TAIL);
        flexPushSigned(&wide, 99, FLEX_ENDPOINT_TAIL);
        assert(flexBytes(wide) > 64);

        cleaner(flexFree) flex *outer = flexNew();
        flexPushBytes(&outer, "before", strlen("before"), FLEX_ENDPOINT_TAIL);
        const databox nested = {.data.bytes.start = inner,
                                .type = DATABOX_CONTAINER_FLEX_MAP,
                                .len = flexBytes(inner)};
        flexPushByType(&outer, &nested, FLEX_ENDPOINT_TAIL);
        const databox nestedWide = {.data.bytes.start = wide,
                                    .type = DATABOX_CONTAINER_FLEX_MAP,
                                    .len = flexBytes(wide)};
        flexPushByType(&outer, &nestedWide, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&outer, 7, FLEX_ENDPOINT_TAIL);

        /* Each container must read back whole, from the head (forward
         * sizing) and from the tail (reverse trailer). */
        if (flexCount(outer) != 4) {
            printf("  ERROR! nested: expected 4 entries, found %zu\n",
                   flexCount(outer));
            failures++;
        } else {
            failures += flexValidateSweepCheckNested(outer, 1, inner);
            failures += flexValidateSweepCheckNested(outer, -3, inner);
            failures += flexValidateSweepCheckNested(outer, 2, wide);
            failures += flexValidateSweepCheckNested(outer, -2, wide);
        }

        failures += flexValidateTotalitySweepOne("nested", outer, 0, 0);
    }

    {
        /* Large enough that the header's total-bytes varint is
         * multi-byte. */
        cleaner(flexFree) flex *big = flexNew();
        for (int32_t i = 0; i < 64; i++) {
            flexPushSigned(&big, i * 1000, FLEX_ENDPOINT_TAIL);
        }
        assert(FLEX_TOTAL_BYTES_WIDTH(big) > 1);
        failures += flexValidateTotalitySweepOne("wide-header", big, 0, 0);
    }

    if (failures == 0) {
        printf("=== flex validator totality sweep passed! ===\n\n");
    } else {
        printf("=== flex validator totality sweep FAILED (%" PRId32
               " failure(s)) ===\n\n",
               failures);
    }

    return failures;
}

#undef FLEX_SWEEP_BYTE_VALUES
#undef FLEX_SWEEP_PEEK_SLACK

/* Total order over flexTestWide values, for qsort. */
static int flexTestWideCmp(const void *a_, const void *b_) {
    const flexTestWide *a = a_;
    const flexTestWide *b = b_;
    if (a->negative != b->negative) {
        return a->negative ? -1 : 1;
    }

    if (a->negative) {
        const __int128_t x = (__int128_t)a->bits;
        const __int128_t y = (__int128_t)b->bits;
        return (x > y) - (x < y);
    }

    return (a->bits > b->bits) - (a->bits < b->bits);
}

static uint64_t flexTestWideRand(uint64_t *state) {
    /* xorshift64*: deterministic, independent of the global rand() seed */
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return *state * UINT64_C(2685821657736338717);
}

/* Sorted insert and find over 64 distinct 96/128-bit keys mixed with
 * 64-bit keys, against a qsort oracle; then a 2-element map whose
 * duplicate narrow keys are ordered by wide values. */
/* Sorted [reference, value] rows whose keys are atoms for 96/128-bit and
 * narrow values.  Every insert and find probes through the reference
 * container, so each probe resolves a wide atom into the probe's slot;
 * a type-only resolve would make databoxCompare read through NULL. */
static int32_t flexTestWideReferenceSorted(void) {
    int32_t bad = 0;
    enum { NARROW_REF_KEYS = 4, REF_KEYS_MAX = 16 };
    flexTestWide keys[REF_KEYS_MAX];
    size_t n = flexTestWideFixedSet(keys);
    keys[n++] = flexTestWideS(-5);
    keys[n++] = flexTestWideU(7);
    keys[n++] = flexTestWideU((uint64_t)1 << 40);
    keys[n++] = flexTestWideS(INT64_MIN);
    assert(n <= REF_KEYS_MAX);

    cleaner(multimapAtomFree) multimapAtom *atoms = multimapAtomNew();
    cleaner(flexFree) flex *f = flexNew();
    flexEntry *middle = f + FLEX_EMPTY_SIZE;

    /* Insert in a fixed scrambled order (7 is coprime with n). */
    for (size_t i = 0; i < n; i++) {
        const size_t k = (i * 7) % n;
        databoxBig key;
        flexTestWideBox(keys[k], false, &key);
        databox ref = *(const databox *)&key;
        multimapAtomInsertConvert(atoms, &ref);
        const databox val = databoxNewUnsigned(k);
        const databox *row[2] = {(const databox *)&key, &val};
        flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
            &f, 2, row, &ref, &middle, true, atoms);
    }

    if (flexCount(f) != 2 * n) {
        printf("  FAIL: reference-sorted flex holds %zu of %zu elements\n",
               flexCount(f), 2 * n);
        return bad + 1;
    }

    /* Keys resolve in ascending order. */
    flexTestWide sorted[REF_KEYS_MAX];
    memcpy(sorted, keys, n * sizeof(keys[0]));
    qsort(sorted, n, sizeof(sorted[0]), flexTestWideCmp);
    size_t i = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe && i < n;
         fe = flexNext(f, flexNext(f, fe)), i++) {
        databox got;
        databoxWide slot;
        flexGetByTypeIntoWithReference(fe, &got, &slot, atoms);
        if (!flexTestWideMatches(&got, sorted[i])) {
            printf("  FAIL: reference-sorted position %zu holds the wrong "
                   "key\n",
                   i);
            bad++;
        }
    }

    /* Every key is found through the references, with its own value. */
    for (i = 0; i < n; i++) {
        databoxBig key;
        flexTestWideBox(keys[i], true, &key);
        flexEntry *const found = flexFindByTypeSortedWithMiddleWithReference(
            f, 2, (const databox *)&key, middle, atoms);
        databox val = {{0}};
        if (found) {
            flexGetByType(flexNext(f, found), &val);
        }

        if (!found || val.type != DATABOX_UNSIGNED_64 || val.data.u64 != i) {
            printf("  FAIL: reference-sorted find of key %zu\n", i);
            bad++;
        }
    }

    return bad;
}

static int32_t flexTestWideSorted(void) {
    int32_t bad = 0;
    enum { WIDE_KEYS = 64, NARROW_KEYS = 16, KEYS = WIDE_KEYS + NARROW_KEYS };
    flexTestWide keys[KEYS];
    size_t n = flexTestWideFixedSet(keys);
    uint64_t state = UINT64_C(0x9E3779B97F4A7C15);

    while (n < KEYS) {
        const bool wantWide = n < WIDE_KEYS;
        __uint128_t r = ((__uint128_t)flexTestWideRand(&state) << 64) |
                        flexTestWideRand(&state);
        flexTestWide v;
        if (!wantWide) {
            v = (r & 1) ? flexTestWideS((int64_t)(uint64_t)r)
                        : flexTestWideU((uint64_t)(r >> 64));
        } else {
            if (r & 2) {
                /* about half are 96-bit */
                r &= ((__uint128_t)1 << 96) - 1;
            }
            v = (r & 1) ? flexTestWideS(-(__int128_t)(r >> 2) - INT64_MAX)
                        : flexTestWideU(r | ((__uint128_t)1 << 64));
        }

        bool fresh = flexTestWideIsWide(v) == wantWide;
        for (size_t j = 0; fresh && j < n; j++) {
            fresh = flexTestWideCmp(&keys[j], &v) != 0;
        }

        if (fresh) {
            keys[n++] = v;
        }
    }

    /* Insert in a shuffled order. */
    size_t order[KEYS];
    for (size_t i = 0; i < KEYS; i++) {
        order[i] = i;
    }

    for (size_t i = KEYS - 1; i > 0; i--) {
        const size_t j = flexTestWideRand(&state) % (i + 1);
        const size_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }

    cleaner(flexFree) flex *f = flexNew();
    for (size_t i = 0; i < KEYS; i++) {
        databoxBig k;
        flexTestWideBox(keys[order[i]], i % 3 == 0, &k);
        flexInsertByTypeSorted(&f, (const databox *)&k);
    }

    qsort(keys, KEYS, sizeof(keys[0]), flexTestWideCmp);
    if (flexCount(f) != KEYS) {
        printf("  FAIL: sorted flex holds %zu of %d keys\n", flexCount(f),
               KEYS);
        return bad + 1;
    }

    size_t i = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe), i++) {
        databoxBig b;
        flexGetByTypeBig(fe, &b);
        if (!flexTestWideMatches((const databox *)&b, keys[i])) {
            printf("  FAIL: sorted position %zu holds the wrong key\n", i);
            bad++;
        }
    }

    /* Every key is found and compares equal at its own position. */
    for (i = 0; i < KEYS; i++) {
        databoxBig k;
        flexTestWideBox(keys[i], i % 2 == 0, &k);
        const databox *const kp = (const databox *)&k;
        flexEntry *const found = flexFindByTypeSorted(f, 1, kp);
        databoxBig got;
        if (found) {
            flexGetByTypeBig(found, &got);
        }

        if (!found || !flexTestWideMatches((const databox *)&got, keys[i]) ||
            flexCompareEntries(f, &kp, 1, (int_fast32_t)i) != 0 ||
            (i + 1 < KEYS &&
             flexCompareEntries(f, &kp, 1, (int_fast32_t)i + 1) <= 0)) {
            printf("  FAIL: sorted find of key %zu\n", i);
            bad++;
        }

        /* The next value up, when it is not a key, is not found. */
        flexTestWide absent = keys[i];
        absent.bits += 1;
        bool probe =
            absent.negative ? (__int128_t)absent.bits < 0 : absent.bits != 0;
        for (size_t j = 0; probe && j < KEYS; j++) {
            probe = flexTestWideCmp(&absent, &keys[j]) != 0;
        }

        if (probe) {
            flexTestWideBox(absent, false, &k);
            if (flexFindByTypeSorted(f, 1, (const databox *)&k)) {
                printf("  FAIL: found absent neighbour of key %zu\n", i);
                bad++;
            }
        }
    }

    /* [narrow key, wide value] rows with duplicate keys: the full-width
     * compare decodes a narrow element and then a wide one per probe. */
    flexTestWide wideKeys[WIDE_KEYS];
    size_t wideCount = 0;
    for (i = 0; i < KEYS; i++) {
        if (flexTestWideIsWide(keys[i]) && wideCount < WIDE_KEYS) {
            wideKeys[wideCount++] = keys[i];
        }
    }

    if (wideCount != WIDE_KEYS) {
        printf("  FAIL: %zu wide keys generated\n", wideCount);
        return bad + 1;
    }

    enum { ROWS = 32 };
    struct {
        int64_t key;
        flexTestWide val;
    } rows[ROWS];
    for (size_t r = 0; r < ROWS; r++) {
        rows[r].key = (int64_t)(r % 4) - 2;
        rows[r].val = wideKeys[(r * 7) % WIDE_KEYS];
    }

    cleaner(flexFree) flex *m = flexNew();
    flexEntry *mid = NULL;
    for (size_t r = 0; r < ROWS; r++) {
        const size_t at = (r * 13) % ROWS;
        const databox key = databoxNewSigned(rows[at].key);
        databoxBig val;
        flexTestWideBox(rows[at].val, false, &val);
        const databox *row[2] = {&key, (const databox *)&val};
        flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&m, 2, row, &mid,
                                                           false);
    }

    /* Oracle order: key, then value. */
    for (size_t a = 1; a < ROWS; a++) {
        for (size_t b = a; b > 0; b--) {
            const bool less =
                rows[b].key < rows[b - 1].key ||
                (rows[b].key == rows[b - 1].key &&
                 flexTestWideCmp(&rows[b].val, &rows[b - 1].val) < 0);
            if (!less) {
                break;
            }

            const __typeof__(rows[0]) t = rows[b];
            rows[b] = rows[b - 1];
            rows[b - 1] = t;
        }
    }

    if (flexCount(m) != ROWS * 2) {
        printf("  FAIL: row map holds %zu elements\n", flexCount(m));
        return bad + 1;
    }

    flexEntry *fe = flexHead(m);
    for (size_t r = 0; r < ROWS; r++) {
        databoxBig k;
        databoxBig v;
        flexGetByTypeBig(fe, &k);
        fe = flexNext(m, fe);
        flexGetByTypeBig(fe, &v);
        fe = flexNext(m, fe);
        if (k.type !=
                (rows[r].key < 0 ? DATABOX_SIGNED_64 : DATABOX_UNSIGNED_64) ||
            k.data.i64 != rows[r].key ||
            !flexTestWideMatches((const databox *)&v, rows[r].val)) {
            printf("  FAIL: row %zu out of order\n", r);
            bad++;
        }

        const databox key = databoxNewSigned(rows[r].key);
        databoxBig val;
        flexTestWideBox(rows[r].val, true, &val);
        const databox *row[2] = {&key, (const databox *)&val};
        flexEntry *const found = flexFindByTypeSortedWithMiddleFullWidth(
            m, 2, row, flexMiddle(m, 2));
        databoxBig got;
        if (found) {
            flexGetByTypeBig(flexNext(m, found), &got);
        }

        if (!found ||
            !flexTestWideMatches((const databox *)&got, rows[r].val)) {
            printf("  FAIL: full-width find of row %zu\n", r);
            bad++;
        }
    }

    return bad;
}

/* Push a FLEX_NEG_64B entry that stores 'magnitude' as given.  The
 * encoder never writes a magnitude above INT64_MAX (it stores those
 * values as FLEX_NEG_96B), so these bytes are written directly. */
static flexEntry *flexTestPushNeg64(flex **const ff, const uint64_t magnitude) {
    flexPushSigned(ff, INT64_MIN, FLEX_ENDPOINT_TAIL);
    flexEntry *const fe = flexTail(*ff);
    assert(fe[0] == FLEX_NEG_64B);
    varintExternalPutFixedWidthQuick_(fe + 1, magnitude, VARINT_WIDTH_64B);
    return fe;
}

/* The value a FLEX_NEG_64B magnitude stands for. */
static flexTestWide flexTestNeg64Value(const uint64_t magnitude) {
    return flexTestWideS(-(__int128_t)magnitude - 1);
}

/* FLEX_NEG_64B entries whose magnitude has bit 63 set hold values in
 * [-2^64, -2^63 - 1].  Every reader decodes them as that value. */
static int32_t flexTestNeg64Wide(void) {
    int32_t bad = 0;
    const uint64_t top = UINT64_C(1) << 63;
    const uint64_t mags[] = {top, top + 1, top + 12345, UINT64_MAX - 1,
                             UINT64_MAX};
    const size_t magCount = sizeof(mags) / sizeof(mags[0]);

    for (size_t i = 0; i < magCount; i++) {
        const flexTestWide want = flexTestNeg64Value(mags[i]);
        /* What a 64-bit decode of these bytes would report. */
        const int64_t wrapped = (int64_t)~mags[i];

        cleaner(flexFree) flex *f = flexNew();
        flexEntry *fe = flexTestPushNeg64(&f, mags[i]);
        if (!flexValidateBufferDeep(f, flexBytes(f))) {
            printf("  FAIL: magnitude %zu rejected by deep validation\n", i);
            bad++;
            continue;
        }

        databoxBig b;
        memset(&b, 0xFF, sizeof(b));
        flexGetByTypeBig(fe, &b);
        if (!flexTestWideMatches((const databox *)&b, want) || !b.big ||
            b.data.ptr != (void *)b.extra || b.allocated) {
            printf("  FAIL: magnitude %zu flexGetByTypeBig\n", i);
            bad++;
        }

        databox plain = {{0}};
        flexGetByType(fe, &plain);
        if (plain.type != DATABOX_SIGNED_128 || plain.data.ptr) {
            printf("  FAIL: magnitude %zu plain decode is not type-only\n", i);
            bad++;
        }

        int64_t s = 0;
        uint64_t u = 0;
        if (flexGetSigned(fe, &s) || flexGetUnsigned(fe, &u)) {
            printf("  FAIL: magnitude %zu read as a 64-bit value\n", i);
            bad++;
        }

        databox copy = {{0}};
        flexGetByTypeCopy(fe, &copy);
        if (!flexTestWideMatches(&copy, want) || !copy.allocated || !copy.big) {
            printf("  FAIL: magnitude %zu flexGetByTypeCopy\n", i);
            bad++;
        }

        databoxFreeData(&copy);

        if (flexCompareSigned(fe, wrapped) ||
            flexCompareUnsigned(fe, (uint64_t)wrapped) ||
            flexFindSigned(f, flexHead(f), wrapped, 0) ||
            flexFindUnsigned(f, flexHead(f), (uint64_t)wrapped, 0)) {
            printf("  FAIL: magnitude %zu matched a 64-bit value\n", i);
            bad++;
        }

        if (flexIncrbySigned(&f, fe, 1, NULL) ||
            flexIncrbyUnsigned(&f, fe, 1, NULL)) {
            printf("  FAIL: magnitude %zu took a 64-bit increment\n", i);
            bad++;
        }

        databox one = {{0}};
        databoxBig oneBig;
        memset(&oneBig, 0xFF, sizeof(oneBig));
        if (flexGetSingleDatabox(f, flexBytes(f), &one) ||
            !flexGetSingleDataboxBig(f, flexBytes(f), &oneBig) ||
            !flexTestWideMatches((const databox *)&oneBig, want)) {
            printf("  FAIL: magnitude %zu single-databox decode\n", i);
            bad++;
        }

        /* The canonical encoding of the same value compares equal. */
        cleaner(flexFree) flex *canon = flexNew();
        databoxBig c;
        flexTestWideBox(want, true, &c);
        flexPushDataboxBig(&canon, &c, FLEX_ENDPOINT_TAIL);
        databoxBig cb;
        flexGetByTypeBig(flexHead(canon), &cb);
        if (flexHead(canon)[0] != FLEX_NEG_96B ||
            databoxCompare((const databox *)&b, (const databox *)&cb) != 0) {
            printf("  FAIL: magnitude %zu differs from its canonical form\n",
                   i);
            bad++;
        }

        flexPushSigned(&f, -1, FLEX_ENDPOINT_TAIL);
        databox pa = {{0}};
        databox pb = {{0}};
        databoxBig ba;
        databoxBig bb;
        if (flexGetTwoDataboxes(f, flexBytes(f), &pa, &pb) ||
            !flexGetTwoDataboxesBig(f, flexBytes(f), &ba, &bb) ||
            !flexTestWideMatches((const databox *)&ba, want) ||
            bb.type != DATABOX_SIGNED_64 || bb.data.i64 != -1) {
            printf("  FAIL: magnitude %zu two-databox decode\n", i);
            bad++;
        }

        /* The 64-bit replace refuses it, as for any wide entry, and the
         * value survives. */
        fe = flexHead(f);
        databoxBig r;
        if (flexReplaceSigned(&f, fe, -5) || flexReplaceUnsigned(&f, fe, 5) ||
            (flexGetByTypeBig(flexHead(f), &r),
             !flexTestWideMatches((const databox *)&r, want))) {
            printf("  FAIL: magnitude %zu took a 64-bit replace\n", i);
            bad++;
        }
    }

    /* Sorted: crafted entries among canonical wide and narrow values. */
    struct {
        flexTestWide v;
        uint64_t neg64; /* nonzero: store as this FLEX_NEG_64B magnitude */
    } rows[] = {
        {flexTestNeg64Value(UINT64_MAX), UINT64_MAX},
        {flexTestWideS(-((__int128_t)1 << 64) + 7), 0},
        {flexTestNeg64Value(top + 12345), top + 12345},
        {flexTestWideS(-((__int128_t)1 << 63) - 2), 0},
        {flexTestNeg64Value(top), top},
        {flexTestWideS(INT64_MIN), 0},
        {flexTestWideS(-1), 0},
        {flexTestWideU(0), 0},
        {flexTestWideU(5), 0},
        {flexTestWideU((__uint128_t)1 << 64), 0},
    };
    const size_t rowCount = sizeof(rows) / sizeof(rows[0]);

    cleaner(flexFree) flex *f = flexNew();
    for (size_t i = 0; i < rowCount; i++) {
        if (rows[i].neg64) {
            flexTestPushNeg64(&f, rows[i].neg64);
        } else {
            databoxBig k;
            flexTestWideBox(rows[i].v, false, &k);
            flexPushDataboxBig(&f, &k, FLEX_ENDPOINT_TAIL);
        }
    }

    for (size_t i = 0; i < rowCount; i++) {
        databoxBig k;
        flexTestWideBox(rows[i].v, true, &k);
        const databox *const kp = (const databox *)&k;
        if (flexFindByTypeSorted(f, 1, kp) != flexIndex(f, (int32_t)i) ||
            flexCompareEntries(f, &kp, 1, (int_fast32_t)i) != 0 ||
            (i > 0 &&
             flexCompareEntries(f, &kp, 1, (int_fast32_t)i - 1) >= 0)) {
            printf("  FAIL: sorted find of row %zu\n", i);
            bad++;
        }
    }

    const flexTestWide absent[] = {
        flexTestWideS(-((__int128_t)1 << 64) + 1),
        flexTestWideS(-((__int128_t)1 << 63) - 12345),
        flexTestWideS(-((__int128_t)1 << 63) - 1 - 12346),
        flexTestWideS((__int128_t)INT64_MIN + 1),
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        databoxBig k;
        flexTestWideBox(absent[i], false, &k);
        if (flexFindByTypeSorted(f, 1, (const databox *)&k)) {
            printf("  FAIL: sorted find of absent value %zu\n", i);
            bad++;
        }
    }

    /* Sorted inserts land between the crafted entries. */
    const flexTestWide added[] = {
        flexTestWideS(-((__int128_t)1 << 64) + 1),
        flexTestWideS(-((__int128_t)1 << 63) - 100),
        flexTestWideS((__int128_t)INT64_MIN + 1),
    };
    flexTestWide oracle[sizeof(rows) / sizeof(rows[0]) +
                        sizeof(added) / sizeof(added[0])];
    size_t oracleCount = 0;
    for (size_t i = 0; i < rowCount; i++) {
        oracle[oracleCount++] = rows[i].v;
    }

    for (size_t i = 0; i < sizeof(added) / sizeof(added[0]); i++) {
        databoxBig k;
        flexTestWideBox(added[i], false, &k);
        flexInsertByTypeSorted(&f, (const databox *)&k);
        oracle[oracleCount++] = added[i];
    }

    qsort(oracle, oracleCount, sizeof(oracle[0]), flexTestWideCmp);
    size_t at = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe), at++) {
        databoxBig b;
        flexGetByTypeBig(fe, &b);
        if (at >= oracleCount ||
            !flexTestWideMatches((const databox *)&b, oracle[at])) {
            printf("  FAIL: sorted insert position %zu\n", at);
            bad++;
        }
    }

    if (at != oracleCount) {
        printf("  FAIL: sorted insert left %zu of %zu entries\n", at,
               oracleCount);
        bad++;
    }

    /* A key-match replace reports a crafted value as type-only. */
    cleaner(flexFree) flex *m = flexNew();
    flexPushSigned(&m, 1, FLEX_ENDPOINT_TAIL);
    flexTestPushNeg64(&m, top);
    flexPushSigned(&m, 2, FLEX_ENDPOINT_TAIL);
    flexTestPushNeg64(&m, UINT64_MAX);
    flexEntry *mid = flexMiddle(m, 2);
    const databox key = databoxNewSigned(1);
    const databox val = databoxNewSigned(7);
    const databox *row[2] = {&key, &val};
    databox replaced = {{0}};
    if (!flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
            &m, 2, row, &mid, true, &replaced) ||
        replaced.type != DATABOX_SIGNED_128 || replaced.data.ptr) {
        printf("  FAIL: replaced crafted value not reported type-only\n");
        bad++;
    }

    return bad;
}

/* Push 'v' between an integer and a string, so the entry under test is
 * neither the first nor the last one the validator walks.  Returns the
 * entry. */
static flexEntry *flexTestPushWideMiddle(flex **const ff,
                                         const flexTestWide v) {
    flexPushSigned(ff, 5, FLEX_ENDPOINT_TAIL);
    databoxBig box;
    flexTestWideBox(v, true, &box);
    flexPushDataboxBig(ff, &box, FLEX_ENDPOINT_TAIL);
    flexPushBytes(ff, "x", 1, FLEX_ENDPOINT_TAIL);
    return flexNext(*ff, flexHead(*ff));
}

/* True when deep validation accepts 'f' and its middle entry decodes to
 * 'want' through both the entry and single-buffer readers. */
static bool flexTestWideMiddleReads(const flex *const f,
                                    const flexTestWide want) {
    if (!flexValidateBufferDeep(f, flexBytes(f))) {
        return false;
    }

    databoxBig b;
    flexGetByTypeBig(flexNext(f, flexHead(f)), &b);
    return flexTestWideMatches((const databox *)&b, want);
}

/* True when the entry at `fe` holds exactly the bytes `s[0..len)`. */
static bool flexTestEntryIsBytes(const flexEntry *fe, const char *s,
                                 size_t len) {
    databoxBig got;
    flexGetByTypeBig(fe, &got);
    const databox *box = (const databox *)&got;
    if (box->type != DATABOX_BYTES && box->type != DATABOX_BYTES_EMBED) {
        return false;
    }

    return databoxLen(box) == len &&
           (len == 0 || memcmp(databoxBytes(box), s, len) == 0);
}

/* The exact path stores every string as the bytes given, numerals and
 * near-numerals included, through each exact entry point.  The auto path
 * still converts a numeral. */
/* Position of 'fe' in 'f' from the head, or -1 for NULL. */
static int32_t flexTestIndexOf(const flex *f, const flexEntry *fe) {
    if (!fe) {
        return -1;
    }

    int32_t i = 0;
    for (flexEntry *at = flexHead(f); at; at = flexNext(f, at), i++) {
        if (at == fe) {
            return i;
        }
    }

    return -2; /* not an entry of 'f' at all (e.g. the flex itself) */
}

/* Every typed find against an oracle of entry positions, in a flex holding
 * one entry of each kind. */
static int32_t flexTestTypedFind(void) {
    int32_t bad = 0;
    cleaner(flexFree) flex *f = flexNew();
    const databox entries[] = {
        databoxNewBytes("", 0),         /* 0 */
        databoxNewBytes("a", 1),        /* 1 */
        databoxNewBytes("22", 2),       /* 2: bytes, stored exactly */
        databoxNewSigned(22),           /* 3 */
        databoxNewSigned(-1),           /* 4 */
        databoxNewUnsigned(UINT64_MAX), /* 5 */
        databoxNewReal(1.5),            /* 6 */
        databoxNewReal(NAN),            /* 7 */
        DATABOX_BOX_TRUE,               /* 8 */
        DATABOX_BOX_FALSE,              /* 9 */
        DATABOX_BOX_NULL,               /* 10 */
    };
    for (size_t i = 0; i < COUNT_ARRAY(entries); i++) {
        flexPushByTypeExact(&f, &entries[i], FLEX_ENDPOINT_TAIL);
    }

    databoxBig wide;
    DATABOX_BIG_UNSIGNED_128(&wide, (__uint128_t)1 << 100);
    flexPushByTypeExact(&f, (const databox *)&wide,
                        FLEX_ENDPOINT_TAIL); /* 11 */

    flexEntry *head = flexHead(f);
    flexEntry *tail = flexTail(f);
    const databox probe22 = databoxNewBytes("22", 2);
    const databox probe15 = databoxNewReal(1.5);
    const databox probeNan = databoxNewReal(-NAN);
    const databox probeMissing = databoxNewReal(2.5);
    const databox probeContainer = {.type = DATABOX_CONTAINER_FLEX_LIST};
    const struct {
        const char *what;
        const flexEntry *got;
        int32_t want;
    } checks[] = {
        {"string \"\"", flexFindString(f, head, "", 0, 0), 0},
        {"string \"\" reverse", flexFindStringReverse(f, tail, "", 0, 0), 0},
        {"string \"22\"", flexFindString(f, head, "22", 2, 0), 2},
        {"signed 22", flexFindSigned(f, head, 22, 0), 3},
        {"unsigned 22", flexFindUnsigned(f, head, 22, 0), 3},
        {"signed -1", flexFindSigned(f, head, -1, 0), 4},
        {"signed -1 reverse", flexFindSignedReverse(f, tail, -1, 0), 4},
        {"unsigned UINT64_MAX", flexFindUnsigned(f, head, UINT64_MAX, 0), 5},
        {"signed -1 past -1",
         flexFindSigned(f, flexNext(f, flexIndex(f, 4)), -1, 0), -1},
        {"by type bytes \"22\"", flexFindByType(f, head, &probe22, 0), 2},
        {"by type 1.5", flexFindByType(f, head, &probe15, 0), 6},
        {"by type NaN", flexFindByType(f, head, &probeNan, 0), 7},
        {"by type TRUE", flexFindByType(f, head, &DATABOX_BOX_TRUE, 0), 8},
        {"by type FALSE", flexFindByType(f, head, &DATABOX_BOX_FALSE, 0), 9},
        {"by type NULL", flexFindByType(f, head, &DATABOX_BOX_NULL, 0), 10},
        {"by type 2^100", flexFindByType(f, head, (const databox *)&wide, 0),
         11},
        {"by type missing 2.5", flexFindByType(f, head, &probeMissing, 0), -1},
        {"by type container", flexFindByType(f, head, &probeContainer, 0), -1},
    };
    for (size_t i = 0; i < COUNT_ARRAY(checks); i++) {
        const int32_t at = flexTestIndexOf(f, checks[i].got);
        if (at != checks[i].want) {
            printf("  FAIL: %s found at %" PRId32 ", expected %" PRId32 "\n",
                   checks[i].what, at, checks[i].want);
            bad++;
        }
    }

    return bad;
}

static int32_t flexTestExactPath(void) {
    int32_t bad = 0;
    static const char *const strs[] = {
        "22",
        "-5",
        "0",
        "007",
        "1e3",
        "22.0",
        "",
        "18446744073709551616",
        "170141183460469231731687303715884105727",
        "hello",
    };

    for (size_t i = 0; i < COUNT_ARRAY(strs); i++) {
        const char *s = strs[i];
        const size_t len = strlen(s);
        const databox box = databoxNewBytes(s, len);
        databoxBig big = {0};
        memcpy(&big, &box, sizeof(box));

        cleaner(flexFree) flex *viaBytes = flexNew();
        flexPushBytesExact(&viaBytes, s, len, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&viaBytes, &box, FLEX_ENDPOINT_HEAD);
        cleaner(flexFree) flex *viaFactory = flexNewFromDataboxExact(&box);
        cleaner(flexFree) flex *viaBig = flexNewFromDataboxBigExact(&big);

        const flex *const all[] = {viaBytes, viaFactory, viaBig};
        for (size_t f = 0; f < COUNT_ARRAY(all); f++) {
            for (flexEntry *fe = flexHead(all[f]); fe;
                 fe = flexNext(all[f], fe)) {
                if (!flexTestEntryIsBytes(fe, s, len)) {
                    printf("  exact path changed \"%s\" (flex %zu)\n", s, f);
                    bad++;
                }
            }
        }
    }

    /* Binary bytes with a NUL that spell no numeral, and bytes that do. */
    {
        static const char bin[] = {'1', '2', '\0', '3'};
        cleaner(flexFree) flex *f = flexNew();
        flexPushBytesExact(&f, bin, sizeof(bin), FLEX_ENDPOINT_TAIL);
        if (!flexTestEntryIsBytes(flexHead(f), bin, sizeof(bin))) {
            printf("  exact path changed a binary blob\n");
            bad++;
        }
    }

    /* Numbers keep their value on the exact path. */
    {
        cleaner(flexFree) flex *f = flexNew();
        const databox neg = DATABOX_SIGNED(-5);
        const databox dbl = DATABOX_DOUBLE(1.5);
        flexPushByTypeExact(&f, &neg, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&f, &dbl, FLEX_ENDPOINT_TAIL);
        databox a = {{0}};
        databox b = {{0}};
        flexGetByType(flexHead(f), &a);
        flexGetByType(flexNext(f, flexHead(f)), &b);
        if (a.type != DATABOX_SIGNED_64 || a.data.i != -5) {
            printf("  exact path changed -5\n");
            bad++;
        }
        const double back =
            b.type == DATABOX_FLOAT_32 ? b.data.f32 : b.data.d64;
        if ((b.type != DATABOX_FLOAT_32 && b.type != DATABOX_DOUBLE_64) ||
            back != 1.5) {
            printf("  exact path changed 1.5\n");
            bad++;
        }
    }

    /* The auto path is unchanged: "22" is stored as the integer 22. */
    {
        cleaner(flexFree) flex *f = flexNew();
        flexPushBytes(&f, "22", 2, FLEX_ENDPOINT_TAIL);
        databox a = {{0}};
        flexGetByType(flexHead(f), &a);
        if (a.type != DATABOX_UNSIGNED_64 || a.data.u != 22) {
            printf("  auto path no longer converts \"22\"\n");
            bad++;
        }
    }

    return bad;
}

/* Negative 96/128-bit magnitudes at the edge of the range.  A
 * FLEX_NEG_128B magnitude above INT128_MAX stands for a value below
 * INT128_MIN; deep validation rejects it.  FLEX_NEG_96B has no
 * out-of-range magnitude. */
static int32_t flexTestNegWideRange(void) {
    int32_t bad = 0;
    const __uint128_t top = (__uint128_t)1 << 127;

    /* INT128_MIN, |v| = 2^127, is the largest magnitude: stored as
     * INT128_MAX. */
    {
        const flexTestWide want = flexTestWideS(INT128_MIN);
        cleaner(flexFree) flex *f = flexNew();
        flexEntry *const fe = flexTestPushWideMiddle(&f, want);
        if (fe[0] != FLEX_NEG_128B ||
            varintBigExternalGet(fe + 1, VARINT_WIDTH_128B) !=
                (__uint128_t)INT128_MAX) {
            printf("  FAIL: INT128_MIN not stored as magnitude INT128_MAX\n");
            bad++;
        }

        if (!flexTestWideMiddleReads(f, want)) {
            printf("  FAIL: INT128_MIN rejected or misread\n");
            bad++;
        }

        databoxBig one;
        cleaner(flexFree) flex *single = flexNew();
        databoxBig box;
        flexTestWideBox(want, true, &box);
        flexPushDataboxBig(&single, &box, FLEX_ENDPOINT_TAIL);
        if (!flexGetSingleDataboxBig(single, flexBytes(single), &one) ||
            !flexTestWideMatches((const databox *)&one, want)) {
            printf("  FAIL: INT128_MIN single-databox decode\n");
            bad++;
        }

        /* Stored 2^127, 2^127 + 1, ..., 2^128 - 1: |v| = 2^127 + 1 and
         * up.  Each is rejected, by the deep check and by the
         * single-buffer reader that runs it. */
        const __uint128_t over[] = {top, top + 1, top + 12345, UINT128_MAX - 1,
                                    UINT128_MAX};
        for (size_t i = 0; i < sizeof(over) / sizeof(over[0]); i++) {
            varintExternalPutFixedWidthBig(fe + 1, over[i], VARINT_WIDTH_128B);
            if (flexValidateBufferDeep(f, flexBytes(f))) {
                printf("  FAIL: NEG_128B magnitude over INT128_MAX (%zu) "
                       "accepted\n",
                       i);
                bad++;
            }

            flexEntry *const sfe = flexHead(single);
            varintExternalPutFixedWidthBig(sfe + 1, over[i], VARINT_WIDTH_128B);
            if (flexGetSingleDataboxBig(single, flexBytes(single), &one)) {
                printf("  FAIL: NEG_128B magnitude over INT128_MAX (%zu) "
                       "decoded as a single databox\n",
                       i);
                bad++;
            }
        }

        /* Below the limit the same entry is accepted again. */
        varintExternalPutFixedWidthBig(fe + 1, (__uint128_t)INT128_MAX - 1,
                                       VARINT_WIDTH_128B);
        if (!flexTestWideMiddleReads(f, flexTestWideS(INT128_MIN + 1))) {
            printf("  FAIL: NEG_128B magnitude INT128_MAX - 1 not "
                   "INT128_MIN + 1\n");
            bad++;
        }

        varintExternalPutFixedWidthBig(fe + 1, (__uint128_t)INT128_MAX,
                                       VARINT_WIDTH_128B);
        if (!flexTestWideMiddleReads(f, want)) {
            printf("  FAIL: restored INT128_MIN rejected or misread\n");
            bad++;
        }
    }

    /* -2^96 is the largest FLEX_NEG_96B magnitude (all twelve payload
     * bytes set), and the next value down needs FLEX_NEG_128B. */
    {
        const __int128_t min96 = -((__int128_t)1 << 96);
        const flexTestWide want = flexTestWideS(min96);
        cleaner(flexFree) flex *f = flexNew();
        const flexEntry *const fe = flexTestPushWideMiddle(&f, want);
        if (fe[0] != FLEX_NEG_96B ||
            varintBigExternalGet(fe + 1, VARINT_WIDTH_96B) !=
                ((__uint128_t)1 << 96) - 1) {
            printf("  FAIL: -2^96 not stored as NEG_96B magnitude 2^96 - 1\n");
            bad++;
        }

        if (!flexTestWideMiddleReads(f, want)) {
            printf("  FAIL: -2^96 rejected or misread\n");
            bad++;
        }

        const flexTestWide below = flexTestWideS(min96 - 1);
        cleaner(flexFree) flex *g = flexNew();
        const flexEntry *const ge = flexTestPushWideMiddle(&g, below);
        if (ge[0] != FLEX_NEG_128B || !flexTestWideMiddleReads(g, below)) {
            printf("  FAIL: -2^96 - 1 not stored as NEG_128B\n");
            bad++;
        }
    }

    return bad;
}

/* Read the middle of a [bytes, integer, bytes] flex forward and in
 * reverse, and check both against 'want'. */
static bool flexTestReplaceReadsBack(flex *f, const flexTestWide want) {
    if (flexCount(f) != 3 || !flexValidateBufferDeep(f, flexBytes(f))) {
        return false;
    }

    databoxBig fwd;
    databoxBig rev;
    flexGetByTypeBig(flexNext(f, flexHead(f)), &fwd);
    flexGetByTypeBig(flexPrev(f, flexTail(f)), &rev);
    return flexTestWideMatches((const databox *)&fwd, want) &&
           flexTestWideMatches((const databox *)&rev, want);
}

/* flexReplaceSigned/Unsigned and flexIncrby* across sign changes, in
 * place (the new value fits the old width) and re-encoded. */
static int32_t flexTestReplaceSign(void) {
    int32_t bad = 0;
    const int64_t starts[] = {-3, 3, -300, 300, INT64_MIN, INT64_MAX, -1, 0};
    const int64_t signedNew[] = {-5,  7,    -1,        0,        -200,
                                 200, -128, INT64_MIN, INT64_MAX};
    const uint64_t unsignedNew[] = {0, 255, 65535, UINT64_MAX};

    for (size_t s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
        for (size_t n = 0; n < sizeof(signedNew) / sizeof(signedNew[0]); n++) {
            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
            flexPushSigned(&f, starts[s], FLEX_ENDPOINT_TAIL);
            flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
            if (!flexReplaceSigned(&f, flexNext(f, flexHead(f)),
                                   signedNew[n]) ||
                !flexTestReplaceReadsBack(f, flexTestWideS(signedNew[n]))) {
                printf("  FAIL: replace %" PRId64 " with signed %" PRId64 "\n",
                       starts[s], signedNew[n]);
                bad++;
            }
        }

        for (size_t n = 0; n < sizeof(unsignedNew) / sizeof(unsignedNew[0]);
             n++) {
            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
            flexPushSigned(&f, starts[s], FLEX_ENDPOINT_TAIL);
            flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
            if (!flexReplaceUnsigned(&f, flexNext(f, flexHead(f)),
                                     unsignedNew[n]) ||
                !flexTestReplaceReadsBack(f, flexTestWideU(unsignedNew[n]))) {
                printf("  FAIL: replace %" PRId64 " with unsigned %" PRIu64
                       "\n",
                       starts[s], unsignedNew[n]);
                bad++;
            }
        }
    }

    const struct {
        int64_t start;
        int64_t by;
    } incrs[] = {{-3, -1}, {3, -10}, {-3, 10}, {-1, 1}, {0, -1}, {-300, 299}};
    for (size_t i = 0; i < sizeof(incrs) / sizeof(incrs[0]); i++) {
        cleaner(flexFree) flex *f = flexNew();
        flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, incrs[i].start, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
        const int64_t want = incrs[i].start + incrs[i].by;
        int64_t got = 0;
        if (!flexIncrbySigned(&f, flexNext(f, flexHead(f)), incrs[i].by,
                              &got) ||
            got != want || !flexTestReplaceReadsBack(f, flexTestWideS(want))) {
            printf("  FAIL: %" PRId64 " incremented by %" PRId64 "\n",
                   incrs[i].start, incrs[i].by);
            bad++;
        }
    }

    /* Increment policy: by zero succeeds unchanged; a result outside the
     * value's range is refused, leaving the value unchanged.  The value
     * sits between "a" and "b", as flexTestReplaceReadsBack expects. */
    {
        cleaner(flexFree) flex *f = flexNew();
        flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -7, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
        int64_t got = 1;
        if (!flexIncrbySigned(&f, flexNext(f, flexHead(f)), 0, &got) ||
            got != -7 || !flexTestReplaceReadsBack(f, flexTestWideS(-7))) {
            printf("  FAIL: signed increment by zero\n");
            bad++;
        }
    }
    {
        const struct {
            int64_t start;
            int64_t by;
        } over[] = {{INT64_MAX, 1}, {INT64_MIN, -1}, {INT64_MAX - 5, 6}};
        for (size_t i = 0; i < sizeof(over) / sizeof(over[0]); i++) {
            cleaner(flexFree) flex *f = flexNew();
            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
            flexPushSigned(&f, over[i].start, FLEX_ENDPOINT_TAIL);
            flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
            if (flexIncrbySigned(&f, flexNext(f, flexHead(f)), over[i].by,
                                 NULL) ||
                !flexTestReplaceReadsBack(f, flexTestWideS(over[i].start))) {
                printf("  FAIL: %" PRId64 " + %" PRId64 " was not refused\n",
                       over[i].start, over[i].by);
                bad++;
            }
        }
    }
    {
        cleaner(flexFree) flex *f = flexNew();
        flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, 5, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, "b", 1, FLEX_ENDPOINT_TAIL);
        uint64_t got = 0;
        if (!flexIncrbyUnsigned(&f, flexNext(f, flexHead(f)), 0, &got) ||
            got != 5 ||
            flexIncrbyUnsigned(&f, flexNext(f, flexHead(f)), -6, NULL) ||
            !flexTestReplaceReadsBack(f, flexTestWideU(5))) {
            printf("  FAIL: unsigned increment by zero or below zero\n");
            bad++;
        }
    }

    return bad;
}

#include "jebuf.h"
#include <locale.h>
/* Feed a deferred wide box (wide type, data.ptr == NULL) to one insert or
 * encode path in a child and report whether the child aborted.  The
 * child turns SIGABRT into a distinct exit status, which keeps the
 * platform crash reporter out of it, and silences its stderr. */
#define FLEX_TEST_DEFERRED_PATHS 4
#define FLEX_TEST_ABORT_STATUS 86
static void flexTestAbortHandler_(int sig) {
    (void)sig;
    _exit(FLEX_TEST_ABORT_STATUS);
}

static bool flexTestDeferredAborts_(int32_t path, databoxType type) {
    fflush(stdout);
    fflush(stderr);
    const pid_t kid = fork();
    if (kid < 0) {
        return false;
    }

    if (kid == 0) {
        (void)signal(SIGABRT, flexTestAbortHandler_);
        if (!freopen("/dev/null", "w", stderr)) {
            _exit(2);
        }

        cleaner(flexFree) flex *f = flexNew();
        flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);
        databoxBig deferred;
        memset(&deferred, 0, sizeof(deferred));
        deferred.type = type;
        databox *const box = (databox *)&deferred;
        const databox *row[1] = {box};
        switch (path) {
        case 0: /* flexInsertByType */
            flexPushByType(&f, box, FLEX_ENDPOINT_TAIL);
            break;
        case 1: /* insertContentsFromBox through replace */
            flexReplaceByType(&f, flexHead(f), box);
            break;
        case 2: /* insertContentsFromBox through a multi-element append */
            flexAppendMultipleExact(&f, 1, row);
            break;
        default: /* flexPushDataboxBig */
            flexPushDataboxBig(&f, &deferred, FLEX_ENDPOINT_TAIL);
            break;
        }

        _exit(0);
    }

    int status = 0;
    if (waitpid(kid, &status, 0) != kid) {
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == FLEX_TEST_ABORT_STATUS;
}

/* The decimal spelling of a numeric box read back from a flex: integers in
 * plain decimal (an independent formatter, for the 128-bit arms too) and
 * reals as StrDoubleFormatToBufNice prints them.  'buf' holds at least 64
 * bytes.  Returns the length, or 0 for a non-numeric box. */
static size_t flexTestSpellNumber_(const databox *box, char *buf) {
    bool negative = false;
    __uint128_t magnitude = 0;
    switch (box->type) {
    case DATABOX_SIGNED_64:
        negative = box->data.i < 0;
        magnitude = negative ? 0 - (uint64_t)box->data.i : box->data.u;
        break;
    case DATABOX_UNSIGNED_64:
        magnitude = box->data.u;
        break;
    case DATABOX_SIGNED_128:
        negative = *box->data.i128 < 0;
        magnitude = negative ? 0 - (__uint128_t)*box->data.i128
                             : (__uint128_t)*box->data.i128;
        break;
    case DATABOX_UNSIGNED_128:
        magnitude = *box->data.u128;
        break;
    case DATABOX_FLOAT_32:
        return StrDoubleFormatToBufNice(buf, 64, box->data.f32);
    case DATABOX_DOUBLE_64:
        return StrDoubleFormatToBufNice(buf, 64, box->data.d64);
    default:
        return 0;
    }

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

    return at;
}

/* Sanitized builds (ASan or TSan, both several times slower per memory
 * access) run the big-memory loops below at smaller sizes, unless
 * TEST_MEMORY_BIG asks for the full sizes. */
#if !defined(TEST_MEMORY_BIG) &&                                               \
    (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
#define FLEX_TEST_SANITIZED 1
#else
#define FLEX_TEST_SANITIZED 0
#endif

int32_t flexTest(int32_t argc, char **argv) {
    setlocale(LC_ALL, "");

    /* If an argument is given, use it as the random seed. */
    if (argc == 4) {
        srand(atoi(argv[3]));
        printf("Running test with random seed of: %d\n", atoi(argv[3]));
    } else if (argc == 5) {
        int s = time(NULL);
        printf("Running test with random seed of: %d\n", s);
        srand(s);
    }

    /* One scratch flex reused across every section below: the cleaner
     * covers whatever the last section left live, so each section ends
     * by either freeing it and NULLing it or leaving a live flex here. */
    cleaner(flexFree) flex *f = NULL;
    flexEntry *fe = NULL;
    (void)f;
    (void)fe;

    printf("Verify decoding never writes through or yields an untrusted "
           "pointer:\n");
    {
        int32_t bad = 0;
        const __uint128_t wideU = (__uint128_t)1 << 80;
        const __int128_t wideS = -((__int128_t)1 << 100);

        cleaner(flexFree) flex *wide = flexNew();
        databoxBig w;
        DATABOX_BIG_UNSIGNED_128(&w, wideU);
        flexPushDataboxBig(&wide, &w, FLEX_ENDPOINT_TAIL);
        DATABOX_BIG_SIGNED_128(&w, wideS);
        flexPushDataboxBig(&wide, &w, FLEX_ENDPOINT_TAIL);
        if (!flexValidateBufferDeep(wide, flexBytes(wide))) {
            printf("  FAIL: 96/128-bit entries failed the deep gate\n");
            bad++;
        }

        /* A plain box reports the type only and leaves the memory its
         * pointer names alone; a databoxBig gets the value. */
        for (int32_t i = 0; i < 2; i++) {
            __int128_t sentinel = 12345;
            databox plain = {.data.i128 = &sentinel};
            flexGetByType(flexIndex(wide, i), &plain);
            const databoxType want =
                i == 0 ? DATABOX_UNSIGNED_128 : DATABOX_SIGNED_128;
            if (plain.type != want || plain.data.ptr || sentinel != 12345) {
                printf("  FAIL: plain box decode of 128-bit entry %" PRId32
                       "\n",
                       i);
                bad++;
            }

            databoxBig big;
            DATABOX_BIG_INIT(&big);
            flexGetByType(flexIndex(wide, i), (databox *)&big);
            if (big.type != want ||
                (i == 0 ? *big.data.u128 != wideU : *big.data.i128 != wideS)) {
                printf("  FAIL: big box decode of 128-bit entry %" PRId32 "\n",
                       i);
                bad++;
            }
        }

        /* The plain-box unwrap helpers refuse a 96/128-bit entry and leave
         * their outputs untouched; the big helper decodes it. */
        cleaner(flexFree) flex *one = flexNewFromDataboxBig(&w);
        databox out = databoxNewSigned(77);
        databoxBig outBig;
        if (flexGetSingleDatabox(one, flexBytes(one), &out) ||
            out.type != DATABOX_SIGNED_64 || out.data.i != 77 ||
            !flexGetSingleDataboxBig(one, flexBytes(one), &outBig) ||
            outBig.type != DATABOX_SIGNED_128 || *outBig.data.i128 != wideS) {
            printf("  FAIL: single unwrap of a 128-bit entry\n");
            bad++;
        }

        cleaner(flexFree) flex *pair = flexNew();
        flexPushSigned(&pair, 5, FLEX_ENDPOINT_TAIL);
        flexPushDataboxBig(&pair, &w, FLEX_ENDPOINT_TAIL);
        databox a = databoxNewSigned(1);
        databox b = databoxNewSigned(2);
        if (flexGetTwoDataboxes(pair, flexBytes(pair), &a, &b) ||
            a.data.i != 1 || b.data.i != 2) {
            printf("  FAIL: pair unwrap of a 128-bit entry\n");
            bad++;
        }

#if FLEX_ENABLE_PTR_MDSC
        /* An MDSC entry is a pointer into the writing process.  Built and
         * read here it is real, but no untrusted gate may accept it. */
        cleaner(mdscfree) mdsc *s = mdscnewlen("pointer payload", 15);
        const databox ptrBox = {.type = DATABOX_PTR_MDSC, .data.ptr = s};
        const databox *ptrRow[1] = {&ptrBox};
        cleaner(flexFree) flex *ptrs = flexNew();
        flexAppendMultipleExact(&ptrs, 1, ptrRow);
        databox local = {{0}};
        flexGetByType(flexHead(ptrs), &local);
        if (local.type != DATABOX_PTR_MDSC || local.data.ptr != s ||
            flexValidateBufferDeep(ptrs, flexBytes(ptrs)) ||
            flexGetSingleDatabox(ptrs, flexBytes(ptrs), &out) ||
            flexGetSingleDataboxBig(ptrs, flexBytes(ptrs), &outBig)) {
            printf("  FAIL: an MDSC entry passed an untrusted gate\n");
            bad++;
        }

        /* Integers whose tag byte is flipped to an MDSC tag: the payload
         * becomes an attacker-chosen address. */
        static const struct {
            uint64_t value;
            flexEncoding integer;
            flexEncoding pointer;
        } retags[] = {
            {UINT64_C(0x2898282a2828), FLEX_UINT_48B, FLEX_EXTERNAL_MDSC_48B},
            {UINT64_C(0x1122334455667788), FLEX_UINT_64B,
             FLEX_EXTERNAL_MDSC_64B},
        };
        for (size_t i = 0; i < COUNT_ARRAY(retags); i++) {
            cleaner(flexFree) flex *retag = flexNew();
            flexPushUnsigned(&retag, retags[i].value, FLEX_ENDPOINT_TAIL);
            flexEntry *const rf = flexHead(retag);
            if (rf[0] != retags[i].integer) {
                printf("  FAIL: retag %zu built encoding %d\n", i, rf[0]);
                bad++;
                continue;
            }
            /* The entry is the last one, so its trailer is the last byte. */
            rf[0] = retags[i].pointer;
            retag[flexBytes(retag) - 1] = retags[i].pointer;
            if (flexValidateBufferDeep(retag, flexBytes(retag))) {
                printf("  FAIL: retag %zu passed the deep gate\n", i);
                bad++;
            } else if (flexGetSingleDatabox(retag, flexBytes(retag), &out)) {
                printf("  FAIL: retag %zu passed the single unwrap\n", i);
                bad++;
            }
        }
#endif

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify container math is exact or refused:\n");
    {
        int32_t bad = 0;
#define FLEX_MATH_CHECK_(cond, what)                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL: %s\n", what);                                      \
            bad++;                                                             \
        }                                                                      \
    } while (0)

        cleaner(flexFree) flex *m = flexNew();
        int64_t s = 77;
        uint64_t u = 77;
        double d = 77;
        float fl = 77;

        /* Empty: the identities */
        FLEX_MATH_CHECK_(flexAddSigned(m, &s) && s == 0 &&
                             flexSubtractUnsigned(m, &u) && u == 0 &&
                             flexMultiplyDouble(m, &d) && d == 1,
                         "empty folds give 0, 0 and 1");

        /* [10, -3, 4]: sum, first minus the rest, product */
        flexPushSigned(&m, 10, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&m, -3, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&m, 4, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(flexAddSigned(m, &s) && s == 11 &&
                             flexSubtractSigned(m, &s) && s == 9 &&
                             flexMultiplySigned(m, &s) && s == -120,
                         "signed folds of [10, -3, 4]");
        u = 77;
        FLEX_MATH_CHECK_(!flexAddUnsigned(m, &u) && u == 77,
                         "a negative entry refused by an unsigned fold");
        FLEX_MATH_CHECK_(flexAddDouble(m, &d) && d == 11 &&
                             flexSubtractFloat(m, &fl) && fl == 9.0f,
                         "integers fold as reals");
        flexFree(m);

        /* The range edges: UINT64_MAX is unsigned only; overflow refuses */
        m = flexNew();
        flexPushUnsigned(&m, UINT64_MAX, FLEX_ENDPOINT_TAIL);
        s = 77;
        FLEX_MATH_CHECK_(flexAddUnsigned(m, &u) && u == UINT64_MAX &&
                             !flexAddSigned(m, &s) && s == 77,
                         "UINT64_MAX fits unsigned only");
        flexPushUnsigned(&m, 1, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(
            !flexAddUnsigned(m, &u) && flexSubtractUnsigned(m, &u) &&
                u == UINT64_MAX - 1,
            "an unsigned sum overflow refused, the difference not");
        flexFree(m);

        m = flexNew();
        flexPushSigned(&m, INT64_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&m, 1, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(!flexSubtractSigned(m, &s) && flexAddSigned(m, &s) &&
                             s == INT64_MIN + 1,
                         "INT64_MIN - 1 refused, INT64_MIN + 1 exact");
        flexFree(m);

        m = flexNew();
        flexPushSigned(&m, (int64_t)1 << 32, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&m, (int64_t)1 << 32, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(!flexMultiplySigned(m, &s) &&
                             flexMultiplyDouble(m, &d) && d == 0x1p64,
                         "a product overflow refused as an integer only");
        flexFree(m);

        /* A 128-bit entry folds by value when it fits */
        m = flexNew();
        databoxBig big;
        DATABOX_BIG_SIGNED_128(&big, -((__int128_t)1 << 70));
        flexPushDataboxBig(&m, &big, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(!flexAddSigned(m, &s) && flexAddDouble(m, &d) &&
                             d == -0x1p70,
                         "a 128-bit entry outside int64 refused");
        flexFree(m);

        /* Reals: a real entry is refused by integer folds; inf refused */
        m = flexNew();
        const databox a = DATABOX_DOUBLE(1.5);
        const databox b = DATABOX_DOUBLE(2.25);
        flexPushByTypeExact(&m, &a, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&m, &b, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(flexAddDouble(m, &d) && d == 3.75 &&
                             flexAddFloat(m, &fl) && fl == 3.75f &&
                             flexMultiplyFloat(m, &fl) && fl == 3.375f &&
                             !flexAddSigned(m, &s),
                         "real folds");
        flexFree(m);

        m = flexNew();
        const databox huge = DATABOX_DOUBLE(1e300);
        flexPushByTypeExact(&m, &huge, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&m, &huge, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(!flexMultiplyDouble(m, &d) && flexAddDouble(m, &d) &&
                             d == 2e300 && !flexAddFloat(m, &fl),
                         "an infinite result refused");

        /* A string is not a number (stored exactly: the auto-converting
         * push would store "7" as the integer 7) */
        const databox seven = databoxNewBytesString("7");
        flexPushByTypeExact(&m, &seven, FLEX_ENDPOINT_TAIL);
        FLEX_MATH_CHECK_(!flexAddDouble(m, &d), "a string refused");
#undef FLEX_MATH_CHECK_

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify inserting or encoding a deferred wide box fails fast:\n");
    {
        int32_t bad = 0;
        for (int32_t path = 0; path < FLEX_TEST_DEFERRED_PATHS; path++) {
            for (int32_t sign = 0; sign < 2; sign++) {
                const databoxType type =
                    sign ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
                if (!flexTestDeferredAborts_(path, type)) {
                    printf("  FAIL: path %" PRId32 " type %d did not abort\n",
                           path, (int)type);
                    bad++;
                }
            }
        }

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify a SIGNED databox round-trips through flexInsertByType at "
           "every width boundary:\n");
    {
        /* Insert each value as a SIGNED_64 box and as its decimal spelling
         * (scanned by StrScanScanReliable); each must read back unchanged.
         * Non-negative values may read back as UNSIGNED_64. */
        const int64_t vals[] = {
            -1,          -2,          -255,          -256,      -257,
            -65535,      -65536,      -65537,        -16777216, -16777217,
            -4294967296, -4294967297, INT64_MIN + 1, INT64_MIN, 0,
            1,           255,         256,           65536,     INT64_MAX,
        };
        int32_t bad = 0;
        for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            cleaner(flexFree) flex *rt = flexNew();
            const databox in = databoxNewSigned(vals[i]);
            flexInsertByType(&rt, FLEX_ENTRY_AFTER_TAIL(rt), &in);
            char spelled[32];
            const int sl =
                snprintf(spelled, sizeof(spelled), "%" PRId64, vals[i]);
            flexPushBytes(&rt, spelled, (size_t)sl, FLEX_ENDPOINT_TAIL);
            for (size_t j = 0; j < 2; j++) {
                databox out = {0};
                flexGetByType(flexIndex(rt, (int32_t)j), &out);
                bool same = false;
                if (out.type == DATABOX_SIGNED_64) {
                    same = out.data.i == vals[i];
                } else if (out.type == DATABOX_UNSIGNED_64) {
                    same = vals[i] >= 0 && out.data.u == (uint64_t)vals[i];
                }
                if (!same) {
                    printf("  FAIL: %" PRId64 " (%s) read back as type %d "
                           "i=%" PRIdMAX " u=%" PRIuMAX "\n",
                           vals[i], j == 0 ? "box" : "bytes", (int)out.type,
                           out.data.i, out.data.u);
                    bad++;
                }
            }
        }
        /* The 128-bit arm at the boundaries where a one-off width choice
         * truncates: just past 2^64 and 2^96 in magnitude, and just below
         * INT64_MIN, where the stored magnitude still fits 64 unsigned
         * bits but the value does not fit an int64_t. */
        const __int128_t bigVals[] = {
            -((__int128_t)1 << 64) - 1, -((__int128_t)1 << 64) - 2,
            -((__int128_t)1 << 96) - 1, -((__int128_t)1 << 96) - 2,
            -((__int128_t)1 << 63) - 1, -((__int128_t)1 << 63) - 2,
            -((__int128_t)1 << 64) + 1, -((__int128_t)1 << 64),
        };
        for (size_t i = 0; i < sizeof(bigVals) / sizeof(bigVals[0]); i++) {
            cleaner(flexFree) flex *rt = flexNew();
            databoxBig in;
            DATABOX_BIG_SIGNED_128(&in, bigVals[i]);
            flexInsertByType(&rt, FLEX_ENTRY_AFTER_TAIL(rt),
                             (const databox *)&in);
            databoxBig out;
            flexGetByTypeBig(flexHead(rt), &out);
            if (out.type != DATABOX_SIGNED_128 ||
                *out.data.i128 != bigVals[i]) {
                printf("  FAIL: 128-bit value %zu read back as type %d\n", i,
                       (int)out.type);
                bad++;
            }

            /* The multi-element writer chooses its width separately. */
            const databox *row[1] = {(const databox *)&in};
            cleaner(flexFree) flex *appended = flexNew();
            flexAppendMultipleExact(&appended, 1, row);
            databoxBig single;
            if (!flexGetSingleDataboxBig(appended, flexBytes(appended),
                                         &single) ||
                single.type != DATABOX_SIGNED_128 ||
                *single.data.i128 != bigVals[i]) {
                printf("  FAIL: appended 128-bit value %zu read back as type "
                       "%d\n",
                       i, (int)single.type);
                bad++;
            }
        }
        if (bad) {
            printf("  %" PRId32 " signed round-trips FAILED\n", bad);
            return bad;
        }
        printf("  ok (%zu values, box and bytes; %zu 128-bit values)\n",
               sizeof(vals) / sizeof(vals[0]),
               sizeof(bigVals) / sizeof(bigVals[0]));
    }

    printf("Verify pushed and inserted numerals store as their canonical "
           "numbers:\n");
    {
        /* Each string goes in through flexPushBytes, flexPushByType with a
         * BYTES box (and a BYTES_EMBED box when it fits) and
         * flexInsertBytes.  A numeral must read back equal to
         * databoxCanonical's result and re-print as its input; anything else
         * must read back as the same bytes.  The spellings cover each
         * integer width the encoder picks, 2^64, 2^96 and the 128-bit
         * limits, reals, and near-numerals on both sides of them. */
        static const char *const spellings[] = {
            "22",
            "0",
            "-5",
            "255",
            "256",
            "-129",
            "65536",
            "9223372036854775807",
            "9223372036854775808",
            "-9223372036854775808",
            "18446744073709551615",
            "18446744073709551616",
            "-9223372036854775809",
            "-18446744073709551616",
            "79228162514264337593543950335",
            "79228162514264337593543950336",
            "-79228162514264337593543950336",
            "170141183460469231731687303715884105727",
            "170141183460469231731687303715884105728",
            "-170141183460469231731687303715884105728",
            "340282366920938463463374607431768211455",
            "340282366920938463463374607431768211456",
            "-170141183460469231731687303715884105729",
            "0018446744073709551616",
            "22.0",
            "-0.5",
            "0.4",
            "299.5",
            "007",
            "-0",
            "+5",
            " 22",
            "1e3",
            "299.5000",
            "-0.0",
            "hello",
        };
        int32_t bad = 0;
        uint32_t numerals = 0;
        uint32_t wide = 0;
        for (size_t i = 0; i < COUNT_ARRAY(spellings); i++) {
            const char *const s = spellings[i];
            const size_t len = strlen(s);
            const databox bytes = {
                .data.bytes.ccstart = s, .type = DATABOX_BYTES, .len = len};
            databoxBig scratch;
            const databox *const expect = databoxCanonical(&bytes, &scratch);

            cleaner(flexFree) flex *pf = flexNew();
            flexPushBytes(&pf, s, len, FLEX_ENDPOINT_TAIL);
            flexPushByType(&pf, &bytes, FLEX_ENDPOINT_TAIL);
            size_t ways = 2;
            if (len <= sizeof(bytes.data.bytes.embed)) {
                databox embed = {{0}};
                embed.type = DATABOX_BYTES_EMBED;
                embed.len = len;
                memcpy(embed.data.bytes.embed, s, len);
                flexPushByType(&pf, &embed, FLEX_ENDPOINT_HEAD);
                ways++;
            }
            flexInsertBytes(&pf, flexHead(pf), s, len);
            ways++;

            if (flexCount(pf) != ways) {
                printf("  FAIL: '%s' made %zu entries, expected %zu\n", s,
                       flexCount(pf), ways);
                bad++;
                continue;
            }

            for (size_t j = 0; j < ways; j++) {
                databox got = {{0}};
                databoxWide slot;
                flexGetByTypeInto(flexIndex(pf, (int32_t)j), &got, &slot);
                bool same;
                if (expect == &bytes) {
                    same = DATABOX_IS_BYTES(&got) && got.len == len &&
                           memcmp(databoxBytes(&got), s, len) == 0;
                } else {
                    char spelled[64];
                    const size_t spelledLen =
                        flexTestSpellNumber_(&got, spelled);
                    same = databoxCompare(&got, expect) == 0 &&
                           spelledLen == len && memcmp(spelled, s, len) == 0;
                }

                if (!same) {
                    printf("  FAIL: '%s' (entry %zu) read back as %s,", s, j,
                           databoxReprStr(&got));
                    printf(" expected %s\n", expect == &bytes
                                                 ? "the same bytes"
                                                 : databoxReprStr(expect));
                    bad++;
                }
            }

            if (expect != &bytes) {
                numerals++;
                wide += expect->type == DATABOX_SIGNED_128 ||
                        expect->type == DATABOX_UNSIGNED_128;
            }
        }

        if (wide != 10 || numerals != 25) {
            printf("  FAIL: %" PRIu32 " numerals (%" PRIu32
                   " wide); expected 25 (10 wide)\n",
                   numerals, wide);
            bad++;
        }

        if (bad) {
            printf("  %" PRId32 " canonical round-trips FAILED\n", bad);
            return bad;
        }
        printf("  ok (%zu spellings, %" PRIu32 " numerals)\n",
               COUNT_ARRAY(spellings), numerals);
    }

    printf("Verify flexFind matches strings by bytes and numbers by value:\n");
    {
        cleaner(flexFree) flex *ff = flexNew();
        flexPushBytes(&ff, "hello", 5, FLEX_ENDPOINT_TAIL); /* 0 */
        flexPushBytes(&ff, "22", 2, FLEX_ENDPOINT_TAIL);    /* 1: 22 */
        const databox raw22 = databoxNewBytesString("22");
        flexInsertByType(&ff, FLEX_ENTRY_AFTER_TAIL(ff), &raw22); /* 2 */
        flexPushBytes(&ff, "18446744073709551616", 20,
                      FLEX_ENDPOINT_TAIL);               /* 3: 2^64 */
        flexPushDouble(&ff, 22.5, FLEX_ENDPOINT_TAIL);   /* 4 */
        flexPushBytes(&ff, "-0", 2, FLEX_ENDPOINT_TAIL); /* 5: bytes */
        flexPushBytes(&ff, "", 0, FLEX_ENDPOINT_TAIL);   /* 6 */
        flexPushByType(&ff, &DATABOX_BOX_TRUE, FLEX_ENDPOINT_TAIL); /* 7 */
        flexPushBytes(&ff, "7", 1, FLEX_ENDPOINT_TAIL);             /* 8: 7 */

        const struct {
            const char *s;
            uint32_t skip;
            int32_t want; /* entry index, or -1 for no match */
        } finds[] = {
            {"hello", 0, 0},
            {"22", 0, 1},
            {"22.0", 0, 1},
            {"18446744073709551616", 0, 3},
            {"22.5", 0, 4},
            {"-0", 0, 5},
            {"", 0, 6},
            {"7", 0, 8},
            {"nothere", 0, -1},
            {"true", 0, -1},
            {"1e3", 0, -1},
            {"0018446744073709551616", 0, -1},
            {"22", 1, 2},
            {"hello", 1, 0},
            {"7", 1, 8},
            {"22.5", 1, 4},
            {"18446744073709551616", 1, -1},
            {"-0", 1, -1},
        };
        int32_t bad = 0;
        for (size_t i = 0; i < COUNT_ARRAY(finds); i++) {
            const flexEntry *const found = flexFind(
                ff, finds[i].s, (uint32_t)strlen(finds[i].s), finds[i].skip);
            const flexEntry *const want =
                finds[i].want < 0 ? NULL : flexIndex(ff, finds[i].want);
            if (found != want) {
                printf("  FAIL: flexFind('%s', skip %" PRIu32
                       ") found the wrong entry\n",
                       finds[i].s, finds[i].skip);
                bad++;
            }
        }

        cleaner(flexFree) flex *empty = flexNew();
        if (flexFind(empty, "x", 1, 0) != NULL) {
            printf("  FAIL: flexFind on an empty flex found an entry\n");
            bad++;
        }

        if (bad) {
            return bad;
        }
        printf("  ok (%zu finds)\n", COUNT_ARRAY(finds));
    }

    printf("Verify a nested container read back with flexGetByType writes "
           "back whole:\n");
    {
        /* Nested sizes on both sides of the 1/2-byte (64/65) and 2/3-byte
         * (16447/16448) boundaries of the size varint the trailer holds.
         * No flex is exactly 65 or 16448 bytes: the byte that would make
         * it so also widens its own total-bytes varint. */
        static const size_t innerSizes[] = {62,    63,    64,    66,   67,
                                            16446, 16447, 16449, 16450};
        enum { ROUND_TRIP_FILL_ = 16448 };
        cleanerName(zfree, Bytes) uint8_t *fill = zmalloc(ROUND_TRIP_FILL_);
        memset(fill, 'n', ROUND_TRIP_FILL_);

        int32_t bad = 0;
        for (size_t s = 0; s < COUNT_ARRAY(innerSizes); s++) {
            /* One string sized near the target, padded with one-byte TRUE
             * entries to land on it exactly. */
            const size_t target = innerSizes[s];
            cleaner(flexFree) flex *inner = NULL;
            for (size_t len = target - 12; len < target && !inner; len++) {
                cleaner(flexFree) flex *candidate = flexNew();
                flexPushBytes(&candidate, fill, len, FLEX_ENDPOINT_TAIL);
                const databox t = {.type = DATABOX_TRUE};
                while (flexBytes(candidate) < target) {
                    flexPushByType(&candidate, &t, FLEX_ENDPOINT_TAIL);
                }

                if (flexBytes(candidate) == target) {
                    inner = candidate;
                    candidate = NULL;
                }
            }

            if (!inner) {
                printf("  FAIL: no nested flex of %zu bytes\n", target);
                bad++;
                continue;
            }

            cleaner(flexFree) flex *outer = flexNew();
            const databox nested = {.data.bytes.start = inner,
                                    .type = DATABOX_CONTAINER_FLEX_MAP,
                                    .len = flexBytes(inner)};
            flexPushByType(&outer, &nested, FLEX_ENDPOINT_TAIL);

            /* The read-back len includes the trailer after the nested
             * flex, so it is larger than the nested flex. */
            databox got = {{0}};
            flexGetByType(flexHead(outer), &got);
            if (got.len <= flexBytes(inner)) {
                printf("  FAIL: %zu: read-back len %zu\n", target,
                       (size_t)got.len);
                bad++;
            }

            /* Push at both ends, insert in the middle, and replace: the
             * first three go through flexInsertByType, the replace through
             * insertContentsFromBox. */
            cleaner(flexFree) flex *copy = flexNew();
            flexPushBytes(&copy, "first", strlen("first"), FLEX_ENDPOINT_TAIL);
            flexPushByType(&copy, &got, FLEX_ENDPOINT_TAIL);
            flexPushByType(&copy, &got, FLEX_ENDPOINT_HEAD);
            flexInsertByType(&copy, flexIndex(copy, 1), &got);
            flexReplaceByType(&copy, flexIndex(copy, 2), &got);

            /* A sorted row goes through insertContentsFromBox too. */
            cleaner(flexFree) flex *sorted = flexNew();
            flexEntry *mid = NULL;
            const databox key = databoxNewSigned(1);
            const databox *row[2] = {&key, &got};
            flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&sorted, 2, row,
                                                               &mid, true);

            if (!flexValidateBufferDeep(copy, flexBytes(copy)) ||
                flexCount(copy) != 4 ||
                !flexValidateBufferDeep(sorted, flexBytes(sorted)) ||
                flexCount(sorted) != 2) {
                printf("  FAIL: %zu: written flexes are malformed\n", target);
                bad++;
                continue;
            }

            /* Every copy reads back whole from the head (forward sizing)
             * and from the tail (the trailer). */
            for (int32_t i = 0; i < 4; i++) {
                bad += flexValidateSweepCheckNested(copy, i, inner);
                bad += flexValidateSweepCheckNested(copy, i - 4, inner);
            }

            bad += flexValidateSweepCheckNested(sorted, 1, inner);
            bad += flexValidateSweepCheckNested(sorted, -1, inner);
        }

        if (bad) {
            printf("  %" PRId32 " container round trips FAILED\n", bad);
            return bad;
        }
        printf("  ok (%zu nested sizes)\n", COUNT_ARRAY(innerSizes));
    }

    printf("Verify external-reference encoding round-trips every tier:\n");
    {
        /* Boundary values across all eight width tiers; the 56/64 pair
         * pins the tier-max subtraction at the encode/decode boundary. */
        const uint64_t refVals[] = {
            0,
            REFERENCE_MAX_8,
            REFERENCE_MAX_8 + 1,
            REFERENCE_MAX_16,
            REFERENCE_MAX_16 + 1,
            REFERENCE_MAX_24 + 1,
            REFERENCE_MAX_32 + 1,
            REFERENCE_MAX_40 + 1,
            REFERENCE_MAX_48,
            REFERENCE_MAX_48 + 1,
            REFERENCE_MAX_56,
            REFERENCE_MAX_56 + 1,
            REFERENCE_MAX_56 + 1000000000ULL,
            REFERENCE_MAX_64 - 1,
            REFERENCE_MAX_64,
        };
        for (size_t i = 0; i < COUNT_ARRAY(refVals); i++) {
            const uint64_t v = refVals[i];
            const flexEncoding enc = flexEncodingReferenceUnsigned(v);
            const uint64_t encoded = flexEncodingReferenceUnsignedEncode(v);
            const uint64_t decoded =
                flexEncodingReferenceUnsignedDecode(enc, encoded);
            if (decoded != v) {
                printf("  ref round-trip FAILED for %" PRIu64
                       " (encoding %d, encoded %" PRIu64 ", decoded %" PRIu64
                       ")\n",
                       v, (int)enc, encoded, decoded);
                assert(NULL && "external reference encode/decode mismatch");
            }
        }
        printf("  SUCCESS\n\n");
    }

    printf("Verify push and insert accept every type flexGetByType "
           "reports:\n");
    {
        /* The types with no flexPush_ helper: references, a plain pointer
         * (stored as an integer), and an MDSC pointer.  Each push must
         * match the bytes flexAppendMultiple writes for the same box, and
         * read back unchanged. */
        int32_t bad = 0;
#if FLEX_ENABLE_PTR_MDSC
        cleaner(mdscfree) mdsc *s = mdscnewlen("pushed pointer", 14);
#endif
        static const uint64_t refs[] = {
            0, 255, 256, 70000, UINT64_C(1) << 40, UINT64_MAX};
        enum { PUSH_TYPE_CASES_ = COUNT_ARRAY(refs) + 2 };
        for (size_t i = 0; i < PUSH_TYPE_CASES_; i++) {
            databox in = {{0}};
            databoxType wantType = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
            if (i < COUNT_ARRAY(refs)) {
                in.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
                in.data.u64 = refs[i];
            } else if (i == COUNT_ARRAY(refs)) {
                in.type = DATABOX_PTR;
                in.data.ptr = &bad;
                wantType = DATABOX_UNSIGNED_64;
            } else {
#if FLEX_ENABLE_PTR_MDSC
                in.type = DATABOX_PTR_MDSC;
                in.data.ptr = s;
                wantType = DATABOX_PTR_MDSC;
#else
                continue;
#endif
            }

            const databox *row[1] = {&in};
            cleaner(flexFree) flex *oracle = flexNew();
            flexAppendMultipleExact(&oracle, 1, row);

            cleaner(flexFree) flex *pushed = flexNew();
            flexPushByType(&pushed, &in, FLEX_ENDPOINT_TAIL);
            cleaner(flexFree) flex *inserted = flexNew();
            flexInsertByType(&inserted, flexHead(inserted), &in);

            /* And the box read back from the push pushes back the same. */
            databox got = {{0}};
            flexGetByType(flexHead(pushed), &got);
            cleaner(flexFree) flex *again = flexNew();
            flexPushByType(&again, &got, FLEX_ENDPOINT_HEAD);

            const size_t oracleBytes = flexBytes(oracle);
            const flex *const writes[3] = {pushed, inserted, again};
            bool sameBytes = true;
            for (size_t w = 0; w < COUNT_ARRAY(writes); w++) {
                sameBytes = sameBytes && flexBytes(writes[w]) == oracleBytes &&
                            memcmp(writes[w], oracle, oracleBytes) == 0;
            }

            const bool sameValue =
                got.type == wantType && got.data.u64 == in.data.u64;
            if (!sameValue || !sameBytes) {
                printf("  FAIL: case %zu (type %d) read back as type %d\n", i,
                       (int)in.type, (int)got.type);
                bad++;
            }
        }

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify both row appends write the bytes of their pushes:\n");
    {
        /* flexAppendMultipleExact is the batched form of flexPushByTypeExact
         * and flexAppendMultiple of flexPushByType: a row of every type
         * (numeral strings included, up to 128 bits), element by element,
         * whole, and three times over (past the auto form's 32-value
         * chunks), must give byte-identical flexes.  Containers that build
         * rows from components rely on this to batch their pushes. */
        int32_t bad = 0;
        static const char longText[] =
            "a string long enough to need a two-byte length prefix, "
            "past sixty-four bytes";
        databoxBig s128;
        databoxBig u128;
        DATABOX_BIG_SIGNED_128(&s128, -((__int128_t)1 << 100));
        DATABOX_BIG_UNSIGNED_128(&u128, ((__uint128_t)1 << 127) + 5);
        databox float32 = {.type = DATABOX_FLOAT_32};
        float32.data.f32 = 0.578125f;
        databox ptr = {.type = DATABOX_PTR};
        ptr.data.ptr = &bad;
        databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        ref.data.u64 = 70000;
        const databox boxes[] = {
            databoxNewSigned(INT64_MIN),
            databoxNewSigned(-1),
            databoxNewSigned(127),
            databoxNewSigned(INT64_MAX),
            databoxNewUnsigned(0),
            databoxNewUnsigned(255),
            databoxNewUnsigned(256),
            databoxNewUnsigned(UINT64_MAX),
            databoxNewReal(0.578125),
            databoxNewReal(1e300),
            databoxNewReal(-0.0),
            float32,
            databoxNewBytesString("hello"),
            databoxNewBytesString("22"),
            databoxNewBytesString("007"),
            databoxNewBytesString("-5"),
            databoxNewBytesString("1.5"),
            databoxNewBytesString("170141183460469231731687303715884105727"),
            databoxNewBytesString("-170141183460469231731687303715884105728"),
            databoxNewBytesString(longText),
            databoxNewBytesAllowEmbed("ab", 2),
            DATABOX_BOX_TRUE,
            DATABOX_BOX_FALSE,
            DATABOX_BOX_NULL,
            *(const databox *)&s128,
            *(const databox *)&u128,
            ptr,
            ref,
        };
        /* Every box, then a NULL value (stored as null) */
        enum { ROW_REPEAT_ = 3 };
        const size_t types = COUNT_ARRAY(boxes) + 1;
        const databox *row[(COUNT_ARRAY(boxes) + 1) * ROW_REPEAT_];
        for (size_t i = 0; i < types * ROW_REPEAT_; i++) {
            row[i] = i % types < COUNT_ARRAY(boxes) ? &boxes[i % types] : NULL;
        }

        size_t converted = 0;
        for (int exact = 0; exact < 2; exact++) {
            void (*push)(flex **, const databox *, flexEndpoint) =
                exact ? flexPushByTypeExact : flexPushByType;
            void (*append)(flex **, uint_fast32_t, const databox *const *) =
                exact ? flexAppendMultipleExact : flexAppendMultiple;
            const char *mode = exact ? "exact" : "auto";

            for (size_t i = 0; i < types; i++) {
                cleaner(flexFree) flex *pushed = flexNew();
                push(&pushed, row[i], FLEX_ENDPOINT_TAIL);
                cleaner(flexFree) flex *appended = flexNew();
                append(&appended, 1, &row[i]);
                if (flexBytes(pushed) != flexBytes(appended) ||
                    memcmp(pushed, appended, flexBytes(pushed)) != 0) {
                    printf("  FAIL: %s element %zu (type %d) appends "
                           "differently\n",
                           mode, i, row[i] ? (int)row[i]->type : -1);
                    bad++;
                }

                /* The auto form really converts some of the strings */
                if (!exact && row[i] && DATABOX_IS_BYTES(row[i])) {
                    databox got = {{0}};
                    flexGetByType(flexHead(appended), &got);
                    converted += !DATABOX_IS_BYTES(&got);
                }
            }

            for (size_t len = types; len <= types * ROW_REPEAT_;
                 len += types * (ROW_REPEAT_ - 1)) {
                cleaner(flexFree) flex *pushedRow = flexNew();
                for (size_t i = 0; i < len; i++) {
                    push(&pushedRow, row[i], FLEX_ENDPOINT_TAIL);
                }

                cleaner(flexFree) flex *appendedRow = flexNew();
                append(&appendedRow, (uint_fast32_t)len, row);
                if (flexBytes(pushedRow) != flexBytes(appendedRow) ||
                    memcmp(pushedRow, appendedRow, flexBytes(pushedRow)) != 0 ||
                    flexCount(appendedRow) != len) {
                    printf("  FAIL: a %s row of %zu appends differently\n",
                           mode, len);
                    bad++;
                }
            }
        }

        /* "22", "-5", "1.5" and both 128-bit numerals; "007" stays bytes */
        if (bad || converted != 5) {
            printf("  %zu numeral strings converted, want 5\n", converted);
            assert(NULL && "a row append differs from its pushes");
        }

        printf("  ok (%zu types, rows of %zu and %zu)\n", types, types,
               types * ROW_REPEAT_);
    }

    printf("Verify a flexRow appends like its pushes and empties itself:\n");
    {
        int32_t bad = 0;
        for (int exact = 0; exact < 2; exact++) {
            cleaner(flexFree) flex *pushed = flexNew();
            const databox v22 = databoxNewBytesString("22");
            const databox five = databoxNewSigned(-5);
            const databox x = databoxNewBytes("x", 1);
            const databox big = databoxNewUnsigned(UINT64_MAX);
            const databox *const order[] = {&v22, &five, &x, &big};
            for (size_t i = 0; i < COUNT_ARRAY(order); i++) {
                if (exact) {
                    flexPushByTypeExact(&pushed, order[i], FLEX_ENDPOINT_TAIL);
                } else {
                    flexPushByType(&pushed, order[i], FLEX_ENDPOINT_TAIL);
                }
            }

            cleaner(flexFree) flex *rowed = flexNew();
            flexRow row = FLEX_ROW_INIT;
            flexRowBytes(&row, "22", 2);
            flexRowSigned(&row, -5);
            flexRowAdd(&row, &x);
            flexRowUnsigned(&row, UINT64_MAX);
            if (exact) {
                flexRowAppendExact(&rowed, &row);
            } else {
                flexRowAppend(&rowed, &row);
            }

            if (row.n != 0 || flexBytes(rowed) != flexBytes(pushed) ||
                memcmp(rowed, pushed, flexBytes(pushed)) != 0) {
                printf("  FAIL: %s row\n", exact ? "exact" : "auto");
                bad++;
            }
        }

        if (bad) {
            assert(NULL && "a flexRow differs from its pushes");
        }

        printf("  ok\n");
    }

    printf("Verify replacing sorted inserts report the overwritten value:\n");
    {
        cleaner(flexFree) flex *m = flexNew();
        flexEntry *mid = NULL;
        const databox key = databoxNewSigned(1);
        databox replaced[1];

        databoxBig wideNeg;
        DATABOX_BIG_SIGNED_128(&wideNeg, -(((__int128_t)1 << 100) + 7));
        databoxBig wideUns;
        DATABOX_BIG_UNSIGNED_128(&wideUns, ((__uint128_t)1 << 100) + 7);
        const databox small = databoxNewSigned(42);
        const databox other = databoxNewSigned(43);
        const databox newKey = databoxNewSigned(2);

        /* (key, value, expected type of the replaced slot) per step. */
        const struct {
            const databox *key;
            const databox *val;
            databoxType replacedType;
        } steps[] = {
            {&key, (const databox *)&wideNeg, DATABOX_VOID},
            {&key, &small, DATABOX_SIGNED_128},
            {&key, (const databox *)&wideUns, DATABOX_UNSIGNED_64},
            {&key, &other, DATABOX_UNSIGNED_128},
            {&newKey, &small, DATABOX_VOID},
        };

        int32_t bad = 0;
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            const databox *row[2] = {steps[i].key, steps[i].val};
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
                &m, 2, row, &mid, true, replaced);
            bool ok = replaced[0].type == steps[i].replacedType;
            if (steps[i].replacedType == DATABOX_SIGNED_128 ||
                steps[i].replacedType == DATABOX_UNSIGNED_128) {
                /* Type only: a databox has no room for the payload. */
                ok = ok && !replaced[0].big && replaced[0].data.ptr == NULL;
            } else if (steps[i].replacedType == DATABOX_UNSIGNED_64) {
                ok = ok && replaced[0].data.u == 42;
            }

            if (!ok) {
                printf("  FAIL: step %zu reported type %d\n", i,
                       (int)replaced[0].type);
                bad++;
            }
        }

        if (flexCount(m) != 4) {
            printf("  FAIL: expected 2 entries of 2 elements, found %zu\n",
                   flexCount(m));
            bad++;
        }

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify replacing inserts accept replacedOut aliasing the new "
           "values:\n");
    {
        /* The exchange idiom: new values in, old values out, same boxes. */
        cleaner(flexFree) flex *m = flexNew();
        flexEntry *mid = NULL;
        const databox key = databoxNewUnsigned(1);
        databox vals[2] = {databoxNewUnsigned(100), databoxNewUnsigned(101)};
        const databox *row[3] = {&key, &vals[0], &vals[1]};

        int32_t bad = 0;
        const bool firstFound =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
                &m, 3, row, &mid, true, vals);
        if (firstFound || vals[0].type != DATABOX_VOID ||
            vals[1].type != DATABOX_VOID) {
            printf("  FAIL: new-key insert reported a replacement\n");
            bad++;
        }

        vals[0] = databoxNewUnsigned(200);
        vals[1] = databoxNewUnsigned(201);
        const bool secondFound =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
                &m, 3, row, &mid, true, vals);
        if (!secondFound || vals[0].type != DATABOX_UNSIGNED_64 ||
            vals[0].data.u != 100 || vals[1].type != DATABOX_UNSIGNED_64 ||
            vals[1].data.u != 101) {
            printf("  FAIL: exchange reported types %d/%d\n", (int)vals[0].type,
                   (int)vals[1].type);
            bad++;
        }

        /* The flex holds the new values. */
        uint64_t stored[3] = {0};
        for (int32_t i = 0; i < 3 && flexCount(m) == 3; i++) {
            databox got = {{0}};
            flexGetByType(flexIndex(m, i), &got);
            stored[i] = got.type == DATABOX_UNSIGNED_64 ? got.data.u : 0;
        }

        if (flexCount(m) != 3 || stored[0] != 1 || stored[1] != 200 ||
            stored[2] != 201) {
            printf("  FAIL: flex holds %" PRIu64 "/%" PRIu64 "/%" PRIu64
                   " in %zu entries\n",
                   stored[0], stored[1], stored[2], flexCount(m));
            bad++;
        }

        if (bad) {
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify flexIndexWithMiddle matches flexIndex at every index:\n");
    {
        /* Sorted 2-element rows with a live maintained middle; sweep
         * every element index in both sign conventions and demand
         * pointer equality with the 2-way flexIndex oracle.  Sizes
         * cover empty, one row, even/odd row counts, and enough rows
         * that all four anchor quadrants are exercised. */
        const int rowCounts[] = {0, 1, 2, 3, 8, 63, 200};
        for (size_t rc = 0; rc < COUNT_ARRAY(rowCounts); rc++) {
            cleaner(flexFree) flex *sf = flexNew();
            for (int r = 0; r < rowCounts[rc]; r++) {
                const databox kbox = databoxNewSigned(r);
                const databox vbox = databoxNewSigned(r * 7);
                const databox *row[2] = {&kbox, &vbox};
                flexEntry *mid = flexMiddle(sf, 2);
                flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&sf, 2, row,
                                                                   &mid, true);
            }

            const int32_t count = (int32_t)flexCount(sf);
            const flexEntry *mid = flexMiddle(sf, 2);
            for (int32_t i = -count - 1; i <= count; i++) {
                flexEntry *want = flexIndex(sf, i);
                flexEntry *got = flexIndexWithMiddle(sf, 2, i, mid);
                if (want != got) {
                    printf("  MISMATCH rows=%d index=%" PRId32
                           " (want %p, got %p)\n",
                           rowCounts[rc], i, (void *)want, (void *)got);
                    assert(NULL && "flexIndexWithMiddle diverged!");
                }
            }
        }
        printf("  SUCCESS\n\n");
    }

    printf(
        "Verify flexPushFloatB16 matches flexInsertFloatB16 at both ends:\n");
    {
        int32_t bad = 0;
        /* 1.5 and -2.0 are exact in bfloat16; 3.14159 rounds. */
        static const float values[] = {1.5f, -2.0f, 3.14159f, 0.0f};
        cleaner(flexFree) flex *pushed = flexNew();
        cleaner(flexFree) flex *inserted = flexNew();
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
            flexPushFloatB16(&pushed, values[i], FLEX_ENDPOINT_TAIL);
            flexInsertFloatB16(&inserted, FLEX_ENTRY_AFTER_TAIL(inserted),
                               values[i]);
        }
        flexPushFloatB16(&pushed, 7.0f, FLEX_ENDPOINT_HEAD);
        flexInsertFloatB16(&inserted, FLEX_ENTRY_HEAD(inserted), 7.0f);

        if (!flexEqual(pushed, inserted)) {
            printf("  push and insert encodings differ\n");
            bad++;
        }

        databox got = {{0}};
        flexGetByType(flexHead(pushed), &got);
        if (got.type != DATABOX_FLOAT_32 || got.data.f32 != 7.0f) {
            printf("  head reads back type %d value %f\n", got.type,
                   (double)got.data.f32);
            bad++;
        }

        flexGetByType(flexIndex(pushed, 1), &got);
        if (got.type != DATABOX_FLOAT_32 || got.data.f32 != 1.5f) {
            printf("  first tail value reads back %f\n", (double)got.data.f32);
            bad++;
        }

        if (bad) {
            assert(NULL && "flexPushFloatB16 mismatch");
        }
        printf("  SUCCESS\n\n");
    }

    printf("Verify flexEqual compares metadata and entries exactly:\n");
    {
        int32_t bad = 0;
        cleaner(flexFree) flex *a = flexNew();
        cleaner(flexFree) flex *b = flexNew();
        if (!flexEqual(a, b) || !flexEqual(a, a)) {
            printf("  empty flexes should be equal\n");
            bad++;
        }

        if (flexEqual(a, NULL) || flexEqual(NULL, a) ||
            !flexEqual(NULL, NULL)) {
            printf("  NULL handling wrong\n");
            bad++;
        }

        flexPushBytes(&a, "abc", 3, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&a, -77, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&b, "abc", 3, FLEX_ENDPOINT_TAIL);
        if (flexEqual(a, b)) {
            printf("  differing counts compared equal\n");
            bad++;
        }

        flexPushSigned(&b, -77, FLEX_ENDPOINT_TAIL);
        if (!flexEqual(a, b)) {
            printf("  identical contents compared unequal\n");
            bad++;
        }

        /* Same length, one byte different. */
        cleaner(flexFree) flex *c = flexNew();
        flexPushBytes(&c, "abd", 3, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&c, -77, FLEX_ENDPOINT_TAIL);
        if (flexBytes(c) != flexBytes(a) || flexEqual(a, c)) {
            printf("  same-size differing contents compared equal\n");
            bad++;
        }

        if (bad) {
            assert(NULL && "flexEqual mismatch");
        }
        printf("  SUCCESS\n\n");
    }

    printf("Verify a value read from a flex pushes onto its own tail:\n");
    {
        /* The pushed box points into the flex it is pushed onto, and the
         * insert position is the flex's end. */
        int32_t bad = 0;
        cleaner(flexFree) flex *self = flexNew();
        flexPushBytes(&self, "hello world", strlen("hello world"),
                      FLEX_ENDPOINT_TAIL);
        databox got = {{0}};
        flexGetByType(flexHead(self), &got);
        flexPushByType(&self, &got, FLEX_ENDPOINT_TAIL);
        for (int32_t i = 0; i < 2 && flexCount(self) == 2; i++) {
            if (!flexCompareString(flexIndex(self, i), "hello world",
                                   strlen("hello world"))) {
                bad++;
            }
        }

        if (flexCount(self) != 2 ||
            !flexValidateBufferDeep(self, flexBytes(self))) {
            bad++;
        }

        /* A nested container pushed onto the flex holding it. */
        cleaner(flexFree) flex *inner = flexNew();
        flexPushBytes(&inner, "inner", strlen("inner"), FLEX_ENDPOINT_TAIL);
        flexPushSigned(&inner, -12345, FLEX_ENDPOINT_TAIL);
        cleaner(flexFree) flex *selfNested = flexNew();
        const databox nested = {.data.bytes.start = inner,
                                .type = DATABOX_CONTAINER_FLEX_MAP,
                                .len = flexBytes(inner)};
        flexPushByType(&selfNested, &nested, FLEX_ENDPOINT_TAIL);
        flexGetByType(flexHead(selfNested), &got);
        flexPushByType(&selfNested, &got, FLEX_ENDPOINT_TAIL);
        if (flexCount(selfNested) != 2 ||
            !flexValidateBufferDeep(selfNested, flexBytes(selfNested))) {
            bad++;
        } else {
            for (int32_t i = 0; i < 2; i++) {
                bad += flexValidateSweepCheckNested(selfNested, i, inner);
                bad += flexValidateSweepCheckNested(selfNested, i - 2, inner);
            }
        }

        if (bad) {
            printf("  FAIL: %" PRId32 " self-push checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify delete cursors across header shrinks:\n");
    {
        /* 7-byte entries cross the 64-byte boundary; 62-byte entries
         * start past 16 KiB and cross both. */
        const int32_t bad =
            flexTestDeleteCursor(5, 20) + flexTestDeleteCursor(60, 280);
        if (bad) {
            printf("  FAIL: %" PRId32 " delete cursor checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify capability-owning decoders at every integer width:\n");
    {
        const int32_t bad = flexTestWideDecoders();
        if (bad) {
            printf("  FAIL: %" PRId32 " wide decoder checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify embedded types:\n");
    {
        const uint32_t numberOfImmediateTypes = 4;
        const uint32_t highestNonStaticNumericType = FLEX_CONTAINER_TUPLE;
        const uint32_t lowestTopDownType = FLEX_BYTES_EMPTY;
        const uint32_t typeCount =
            highestNonStaticNumericType - FLEX_UINT_8B + numberOfImmediateTypes;
        const uint32_t typeCountMax = FLEX_SAME - FLEX_FIXED_START;
        printf("Type range: [%" PRIu32 ", %" PRIu32 "] (+ %" PRIu32
               " top-down types) (%" PRIu32 " total used; %" PRIu32 " "
               "max limit; %" PRIu32 " "
               "remaining)\n\n",
               (uint32_t)FLEX_UINT_8B, highestNonStaticNumericType,
               numberOfImmediateTypes, typeCount, typeCountMax,
               typeCountMax - typeCount);

        if (highestNonStaticNumericType >= lowestTopDownType) {
            printf("Too many types!  Highest grow-up type is bigger than "
                   "lowest top-down type!\n");
            printf("Highest grow up is %" PRIu32
                   ", but lowest top-down is %" PRIu32 "\n",
                   highestNonStaticNumericType, lowestTopDownType);
            assert(highestNonStaticNumericType < lowestTopDownType);
        }

        assert(typeCount <= (FLEX_NULL - FLEX_FIXED_START));
        assert((int)FLEX_NEG_8B > (int)VARINT_SPLIT_FULL_NO_ZERO_BYTE_8);
        /* Verify our our immediate value encodings are at
         * the *maximum* type byte positions to ensure integrity of
         * insert and traversal operations
         * (because we check with (encoding >= FLEX_NULL)) */
        assert(FLEX_BYTES_EMPTY == 252);
        assert(FLEX_TRUE == 253);
        assert(FLEX_FALSE == 254);
        assert(FLEX_NULL == 255);
    }

    printf("Verify sorted insert and find over 96/128-bit keys:\n");
    {
        const int32_t bad = flexTestWideSorted();
        if (bad) {
            printf("  FAIL: %" PRId32 " wide sorted checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify reference-sorted probes resolve 96/128-bit atoms:\n");
    {
        const int32_t bad = flexTestWideReferenceSorted();
        if (bad) {
            printf("  FAIL: %" PRId32 " wide reference checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify NEG_64B magnitudes above INT64_MAX decode as 128-bit:\n");
    {
        const int32_t bad = flexTestNeg64Wide();
        if (bad) {
            printf("  FAIL: %" PRId32 " NEG_64B wide checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify deep validation bounds NEG_96B/NEG_128B magnitudes:\n");
    {
        const int32_t bad = flexTestNegWideRange();
        if (bad) {
            printf("  FAIL: %" PRId32 " negative wide range checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify typed finds match by kind and value:\n");
    {
        const int32_t bad = flexTestTypedFind();
        if (bad) {
            printf("  FAIL: %" PRId32 " typed find checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify the exact push path stores values as given:\n");
    {
        const int32_t bad = flexTestExactPath();
        if (bad) {
            printf("  FAIL: %" PRId32 " exact path checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

    printf("Verify integer replace and increment across sign changes:\n");
    {
        const int32_t bad = flexTestReplaceSign();
        if (bad) {
            printf("  FAIL: %" PRId32 " replace checks\n", bad);
            return bad;
        }
        printf("  ok\n");
    }

#if 1
    int64_t value;
    flexEntry *entry;
    uint32_t elen;

    f = flexNew();
    flexRepr(f);
    flexFree(f);

    f = createIntList();
    flexRepr(f);
    flexFree(f);

    f = createList();
    flexRepr(f);

    pop(&f, FLEX_ENDPOINT_TAIL);
    flexRepr(f);

    pop(&f, FLEX_ENDPOINT_HEAD);
    flexRepr(f);

    pop(&f, FLEX_ENDPOINT_TAIL);
    flexRepr(f);

    pop(&f, FLEX_ENDPOINT_TAIL);
    flexRepr(f);
    flexFree(f);

    printf("Get element at index 3:\n");
    {
        f = createList();
        fe = flexIndexDirect(f, 3);
        if (!flexGet(fe, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            assert(NULL);
        }

        if (entry) {
            if (elen) {
                printf("%.*s", elen, entry);
            }

            printf("\n");
        } else {
            printf("%" PRIi64 "\n", value);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Get element at index 4 (out of range):\n");
    {
        f = createList();
        fe = flexIndexDirect(f, 4);
        if (fe == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned "
                   "offset: %" PRIdPTR "\n",
                   (ptrdiff_t)(fe - f));
            assert(NULL);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Get element at index -1 (last element):\n");
    {
        f = createList();
        fe = flexIndexDirect(f, -1);
        if (!flexGet(fe, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            assert(NULL);
        }

        if (entry) {
            if (elen) {
                printf("%.*s", elen, entry);
            }

            printf("\n");
        } else {
            printf("%" PRIi64 "\n", value);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Get element at index -4 (first element):\n");
    {
        f = createList();
        fe = flexIndexDirect(f, -4);
        if (!flexGet(fe, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            assert(NULL);
        }

        if (entry) {
            if (elen) {
                printf("%.*s", elen, entry);
            }

            printf("\n");
        } else {
            printf("%" PRIi64 "\n", value);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        f = createList();
        fe = flexIndexDirect(f, -5);
        if (fe == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned "
                   "offset: %" PRIdPTR "\n",
                   (ptrdiff_t)(fe - f));
            assert(NULL);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate list from 0 to end:\n");
    {
        f = createList();
        fe = flexIndex(f, 0);
        while (flexGet(fe, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen) {
                    printf("%.*s", elen, entry);
                }
            } else {
                printf("%" PRIi64 "", value);
            }

            fe = flexNext(f, fe);
            printf("\n");
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate list from 1 to end:\n");
    {
        f = createList();
        fe = flexIndex(f, 1);
        while (flexGet(fe, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen) {
                    printf("%.*s", elen, entry);
                }
            } else {
                printf("%" PRIi64 "", value);
            }

            fe = flexNext(f, fe);
            printf("\n");
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate list from 2 to end:\n");
    {
        f = createList();
        fe = flexIndex(f, 2);
        while (flexGet(fe, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen) {
                    printf("%.*s", elen, entry);
                }
            } else {
                printf("%" PRIi64 "", value);
            }

            fe = flexNext(f, fe);
            printf("\n");
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate starting out of range:\n");
    {
        f = createList();
        fe = flexIndex(f, 4);
        if (!flexGet(fe, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
            assert(NULL);
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate from back to front:\n");
    {
        f = createList();
        fe = flexIndex(f, -1);
        assert(fe == flexTail(f));
        while (flexGet(fe, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen) {
                    printf("%.*s", elen, entry);
                }
            } else {
                printf("%" PRIi64 "", value);
            }

            fe = flexPrev(f, fe);
            printf("\n");
        }

        printf("\n");
        flexFree(f);
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        f = createList();
        fe = flexIndex(f, -1);
        assert(fe == flexTail(f));
        while (flexGet(fe, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen) {
                    fwrite(entry, elen, 1, stdout);
                }
            } else {
                printf("%" PRIi64 "", value);
            }

            flexDelete(&f, &fe);
            fe = flexPrev(f, fe);
            printf("\n");
        }

        printf("\n");
        flexFree(f);
    }

    printf("Delete inclusive range 0,0:\n");
    {
        f = createList();
        flexDeleteRange(&f, 0, 1);
        flexRepr(f);
        flexFree(f);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        f = createList();
        flexDeleteRange(&f, 0, 2);
        flexRepr(f);
        flexFree(f);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        f = createList();
        flexDeleteRange(&f, 1, 2);
        flexRepr(f);
        flexFree(f);
    }

    printf("Delete with start index out of range:\n");
    {
        f = createList();
        flexDeleteRange(&f, 5, 1);
        flexRepr(f);
        flexFree(f);
    }

    printf("Delete with num overflow:\n");
    {
        f = createList();
        flexDeleteRange(&f, 1, 5);
        flexRepr(f);
        flexFree(f);
    }

    printf("Delete foo while iterating:\n");
    {
        f = createList();
        fe = flexIndex(f, 0);
        while (flexGet(fe, &entry, &elen, &value)) {
            if (entry && strncmp("foo", (char *)entry, elen) == 0) {
                printf("Delete foo\n");
                flexDelete(&f, &fe);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen) {
                        printf("%.*s", elen, entry);
                    }
                } else {
                    printf("%" PRIi64 "", value);
                }

                fe = flexNext(f, fe);
                printf("\n");
            }
        }
        printf("\n");
        flexRepr(f);
        flexFree(f);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257] = {0};
        char v2[257] = {0};
        entry = NULL;
        elen = 0;
        memset(v1, 'x', 256);
        memset(v2, 'y', 256);
        f = flexNew();
        printf("Pushing v1 to tail...\n");
        flexPushBytes(&f, v1, strlen(v1), FLEX_ENDPOINT_TAIL);
        printf("Pushing v2 to tail...\n");
        flexPushBytes(&f, v2, strlen(v2), FLEX_ENDPOINT_TAIL);

        /* Pop values again and compare their value. */
        fe = flexIndex(f, 0);
        assert(fe == flexHead(f));
        if (entry) {
            assert(flexGet(fe, &entry, &elen, &value));
            assert(strncmp(v1, (char *)entry, elen) == 0);
        }
        fe = flexIndex(f, 1);
        assert(flexGet(fe, &entry, &elen, &value));
        if (entry) {
            assert(strncmp(v2, (char *)entry, elen) == 0);
        }
        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257] = {{0}};
        flexEntryData e[3] = {{0}};

        for (size_t i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][1] = '\0';
        v[2][256] = '\0';

        f = flexNew();
        for (size_t i = 0; i < (sizeof(v) / sizeof(v[0])); i++) {
            flexPushBytes(&f, v[i], strlen(v[i]), FLEX_ENDPOINT_TAIL);
        }

        verify(f, e);

        assert(e[0].encodingSize == 2);
        assert(e[1].encodingSize == 1);
        assert(e[2].encodingSize == 2);

        fe = (flexEntry *)e[1].fe;
        flexDelete(&f, &fe);

        verify(f, e);

        assert(e[0].encodingSize == 2);
        assert(e[1].encodingSize == 2);

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test integer range encodings:\n");
    {
        f = flexNew();
        flexPushSigned(&f, INT8_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT8_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, UINT8_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, (1 << 8) - 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -(1 << 8), FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT16_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT16_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, UINT16_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, (1 << 16) - 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -(1 << 16), FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT32_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT32_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, UINT32_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, (1ULL << 32) - 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -(1LL << 32), FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT64_MIN, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, INT64_MAX, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, UINT64_MAX, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, (1LL << 62) - 1, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, -(1LL << 62), FLEX_ENDPOINT_TAIL);

        flexRepr(f);
        flexFree(f);
    }

    printf("Test 2-level same keys sort sub elements properly:\n");
    {
        f = flexNew();
        const databox samekeybox = databoxBool(false);
        const databox valboxA = databoxNewBytesString("AAAAAAA");
        const databox valboxB = databoxNewBytesString("BBBBBB");
        const databox valboxC = databoxNewBytesString("CCCCCCCC");

        flexEntry *middle = NULL;

        const databox *groupA[2] = {&samekeybox, &valboxA};
        const bool newA = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupA, &middle, false);
        assert(newA == false);

        const databox *groupC[2] = {&samekeybox, &valboxC};
        const bool newC = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupC, &middle, false);
        assert(newC == false);

        const databox *groupB[2] = {&samekeybox, &valboxB};
        const bool newB = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupB, &middle, false);
        assert(newB == false);

        printf("Created A, B, C...\n");
        flexRepr(f);

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test 2-level remove of same keys sort sub elements properly:\n");
    {
        f = flexNew();
        const databox samekeybox = databoxBool(false);
        const databox valboxA = databoxNewBytesString("AAAAAAA");
        const databox valboxB = databoxNewBytesString("BBBBBB");
        const databox valboxC = databoxNewBytesString("CCCCCCCC");

        flexEntry *middle = NULL;

        const databox *groupA[2] = {&samekeybox, &valboxA};
        const bool newA = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupA, &middle, false);
        assert(newA == false);

        const databox *groupC[2] = {&samekeybox, &valboxC};
        const bool newC = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupC, &middle, false);
        assert(newC == false);

        const databox *groupB[2] = {&samekeybox, &valboxB};
        bool newB = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupB, &middle, false);
        assert(newB == false);

        printf("Setup A, B, C...\n");
        flexRepr(f);

        /* Get B */
        flexEntry *foundB =
            flexFindByTypeSortedWithMiddleFullWidth(f, 2, groupB, middle);
        assert(foundB);

        /* Remove B */
        flexDeleteSortedValueWithMiddle(&f, 2, foundB, &middle);

        printf("Removed B...\n");
        flexRepr(f);

        /* Add B back twice */
        newB = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupB, &middle, false);
        assert(newB == false);

        newB = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &f, 2, groupB, &middle, false);
        assert(newB == true);

        printf("Added two Bs...\n");
        flexRepr(f);

        /* Delete *one* B */
        foundB = flexFindByTypeSortedWithMiddleFullWidth(f, 2, groupB, middle);
        assert(foundB);
        flexDeleteSortedValueWithMiddle(&f, 2, foundB, &middle);

        printf("Removed one B...\n");
        flexRepr(f);

        /* Delete *one* C */
        flexEntry *foundC =
            flexFindByTypeSortedWithMiddleFullWidth(f, 2, groupC, middle);
        assert(foundC);
        flexDeleteSortedValueWithMiddle(&f, 2, foundC, &middle);

        printf("Removed C...\n");
        flexRepr(f);

        /* Delete *one* A */
        flexEntry *foundA =
            flexFindByTypeSortedWithMiddleFullWidth(f, 2, groupA, middle);
        assert(foundA);
        flexDeleteSortedValueWithMiddle(&f, 2, foundA, &middle);

        printf("Removed A...\n");
        flexRepr(f);

        /* Delete final B */
        foundB = flexFindByTypeSortedWithMiddleFullWidth(f, 2, groupB, middle);
        assert(foundB);
        flexDeleteSortedValueWithMiddle(&f, 2, foundB, &middle);

        printf("Removed B, now empty...\n");
        flexRepr(f);

        assert(flexCount(f) == 0);
        assert(flexBytes(f) == FLEX_EMPTY_HEADER_SIZE);

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test sorted insert and delete with multiple entries (numeric):\n");
    {
        f = flexNew();
        flexEntry *mid = NULL;

        for (int i = 0; i < 64; i++) {
            /* Test extreme values (near values) */
            const int64_t key = rand() % 2 == 0 ? 0 : -1;
            const int64_t val = rand() % 2 == 0 ? 0 : -1;
#if 0
            printf("[%" PRId64 ", %" PRId64 "]\n", key, val);
#endif
            const databox keybox = databoxNewSigned(key);
            const databox valbox = databoxNewSigned(val);
            const databox *boxes[2] = {&keybox, &valbox};

            /* Note: we use InsertReplace here because we're Inserting
             *       many duplicate keys on this test.  We want them overwritten
             *       and not piled up one after another (which breaks the value
             *       lookups if that happens) */
            flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&f, 2, boxes,
                                                               &mid, true);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));

            assert(flexCount(f) <= (2 * 2));
        }

        for (int i = 0; i < 64; i++) {
            /* Test extreme values (signed) */
            const int64_t key = rand() % 2 == 0 ? INT64_MIN : INT64_MAX;
            const int64_t val = rand() % 2 == 0 ? INT64_MIN : INT64_MAX;
#if 0
            printf("[%" PRId64 ", %" PRId64 "]\n", key, val);
#endif
            const databox keybox = databoxNewSigned(key);
            const databox valbox = databoxNewSigned(val);
            const databox *boxes[2] = {&keybox, &valbox};

            /* Note: we use InsertReplace here because we're Inserting
             *       many duplicate keys on this test.  We want them overwritten
             *       and not piled up one after another (which breaks the value
             *       lookups if that happens) */
            flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&f, 2, boxes,
                                                               &mid, true);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));
        }

        for (int i = 0; i < 64; i++) {
            /* Test extreme values (unsigned) */
            const uint64_t key = rand() % 2 == 0 ? 0 : UINT64_MAX;
            const uint64_t val = rand() % 2 == 0 ? 0 : UINT64_MAX;
#if 0
            printf("[%" PRId64 ", %" PRId64 "]\n", key, val);
#endif
            const databox keybox = databoxNewUnsigned(key);
            const databox valbox = databoxNewUnsigned(val);
            const databox *boxes[2] = {&keybox, &valbox};

            /* Note: we use InsertReplace here because we're Inserting
             *       many duplicate keys on this test.  We want them overwritten
             *       and not piled up one after another (which breaks the value
             *       lookups if that happens) */
            flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&f, 2, boxes,
                                                               &mid, true);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));
        }

        /* reset for clean tests below */
        flexFree(f);
        f = flexNew();
        mid = NULL;

        for (int i = 0; i < 64; i++) {
            const databox keybox = databoxNewSigned(i);
            const databox valbox = databoxNewSigned(i * 100);
            const databox *boxes[2] = {&keybox, &valbox};
            flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));
        }

        for (int i = 0; i < 177; i++) {
            const databox keybox = databoxNewSigned(rand());
            const databox valbox = databoxNewSigned(keybox.data.i * 100);
            const databox *boxes[2] = {&keybox, &valbox};
            flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));
        }

        for (int i = -200; i < 0; i++) {
            const databox keybox = databoxNewSigned(i);
            const databox valbox = databoxNewSigned(i * 100000);
            const databox *boxes[2] = {&keybox, &valbox};
            flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
            assert(mid == flexMiddle(f, 2));
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);

            databox got = {{0}};
            flexGetByType(found, &got);
            assert(databoxEqual(&keybox, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&valbox, &got2));
        }

        cleaner(flexFree) flex *split = flexDuplicate(f);
        flex *secondHalf = flexSplit(&split, 2);

        flexFree(secondHalf);

        for (int i = -200; i < 64; i++) {
            const databox keybox = databoxNewSigned(i);
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(found);
            flexDeleteSortedValueWithMiddle(&f, 2, found, &mid);
            assert(mid == flexMiddle(f, 2));
            found = flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
            assert(mid == flexMiddle(f, 2));
            if (found) {
                databox got = {{0}};
                flexGetByType(found, &got);
                assert(!found);
            }
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test sorted insert and delete with multiple entries (strings):\n");
    {
        /* Run these test with:
         *   - FORWARD delete (-200 to 700)
         *   - REVERSE delete (700 to -200) */
        for (int forward = 0; forward < 2; forward++) {
            f = flexNew();
            flexEntry *mid = NULL;
            for (int i = 0; i < 64; i++) {
                char *k = genkey("key", i);
                char *v =
                    genval("lowVal", i * ((rand() % 2 == 0) ? 100 : 100000));
                const databox keybox = databoxNewBytes(k, strlen(k));
                const databox valbox = databoxNewBytes(v, strlen(v));
                const databox *boxes[2] = {&keybox, &valbox};
                flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
                assert(mid == flexMiddle(f, 2));
                flexEntry *found =
                    flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
                assert(found);
                databox got = {{0}};
                flexGetByType(found, &got);
                assert(databoxEqual(&keybox, &got));
            }

            for (int i = 64; i < 700; i++) {
                char *k = genkey("key", i);
                char *v =
                    genval("UpperVal", i * ((rand() % 2 == 0) ? 1 : 1000));
                const databox keybox = databoxNewBytes(k, strlen(k));
                const databox valbox = databoxNewBytes(v, strlen(v));
                const databox *boxes[2] = {&keybox, &valbox};
                flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
                assert(mid == flexMiddle(f, 2));
                flexEntry *found =
                    flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
                assert(found);
                databox got = {{0}};
                flexGetByType(found, &got);
                assert(databoxEqual(&keybox, &got));
            }

            for (int i = -200; i < 0; i++) {
                char *k = genkey("key", i);
                char *v = genval("überlowerVal",
                                 i * ((rand() % 2 == 0) ? -10000 : -1000000));
                const databox keybox = databoxNewBytes(k, strlen(k));
                const databox valbox = databoxNewBytes(v, strlen(v));
                const databox *boxes[2] = {&keybox, &valbox};
                flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, boxes, &mid);
                assert(mid == flexMiddle(f, 2));
                flexEntry *found =
                    flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
                assert(found);
                databox got = {{0}};
                flexGetByType(found, &got);
                assert(databoxEqual(&keybox, &got));
            }

            cleaner(flexFree) flex *split = flexDuplicate(f);
            flex *secondHalf = flexSplit(&split, 2);

            flexFree(secondHalf);

            for (int i = forward ? -200 : 699; forward ? i < 700 : i >= -200;
                 forward ? i++ : i--) {
                char *k = genkey("key", i);
                const databox keybox = databoxNewBytes(k, strlen(k));
                flexEntry *found =
                    flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
                assert(found);
                flexDeleteSortedValueWithMiddle(&f, 2, found, &mid);
                assert(mid == flexMiddle(f, 2));
                found = flexFindByTypeSortedWithMiddle(f, 2, &keybox, mid);
                assert(mid == flexMiddle(f, 2));
                if (found) {
                    databox got = {{0}};
                    flexGetByType(found, &got);
                    assert(!found);
                }
            }

            assert(flexCount_(f) == 0);
            assert(flexTotalBytes_(f) == FLEX_EMPTY_HEADER_SIZE);

            flexFree(f);
        }

        printf("SUCCESS\n\n");
    }

    printf("Test flexInsertReplaceByTypeSortedWithMiddleMultiWithReference:\n");
    {
        multimapAtom *a = multimapAtomNew();

        f = flexNew();
        flexEntry *middle = f + FLEX_EMPTY_SIZE;

        /* Insert one element */
        databox key = databoxNewBytes(genkey("key", 9000), 32);
        databox insertKey = key;
        databox val = databoxNewBytes(genval("val", 9000), 32);

        /* We re-use this same box array of databoxes throughout since
         * 'key' and 'val' just continue getting repopulated with new values. */
        const databox *boxes[2] = {&key, &val};

        /* Turn key into atom */
        multimapAtomInsertConvert(a, &insertKey);

        /* Insert atom -> value */
        flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
            &f, 2, boxes, &insertKey, &middle, true, a);

        /* Verify atom created properly in map */

        /* Verify map has proper reference */

        /* Insert 18 more elements */
        for (size_t i = 0; i < 18; i++) {
            key = databoxNewBytes(genkey("key", 9001 + i), 32);
            insertKey = key;
            val = databoxNewBytes(genval("val", 9001 + i), 32);

            multimapAtomInsertConvert(a, &insertKey);
            flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
                &f, 2, boxes, &insertKey, &middle, true, a);
        }

        /* Verify atom map */

        /* Lookup 19 elements */
        for (size_t i = 0; i < 19; i++) {
            key = databoxNewBytes(genkey("key", 9000 + i), 32);
            val = databoxNewBytes(genval("val", 9000 + i), 32);
            flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
                f, 2, &key, middle, a);
            assert(found);

            databox got = {{0}};
            flexGetByTypeWithReference(found, &got, a);
            assert(databoxEqual(&key, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&val, &got2));
        }

        assert(flexCount(f) == (2 * 19));

        /* Overwrite 18 elements using discovered pre-existing IDs */
        for (size_t i = 0; i < 18; i++) {
            key = databoxNewBytes(genkey("key", 9001 + i), 32);
            insertKey = key;
            val = databoxNewBytes(genval("valNEWER", 9001 + i), 32);

            multimapAtomInsertIfNewConvert(a, &insertKey);
            flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
                &f, 2, boxes, &insertKey, &middle, true, a);

            assert(middle == flexMiddle(f, 2));

            flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
                f, 2, &key, middle, a);

            assert(found);
            databox got = {{0}};
            flexGetByTypeWithReference(found, &got, a);
            assert(databoxEqual(&key, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&val, &got2));
        }

        assert(flexCount(f) == (2 * 19));

        /* Convert 18 element values to atoms too! */
        for (size_t i = 0; i < 18; i++) {
            key = databoxNewBytes(genkey("key", 9001 + i), 32);
            insertKey = key;
            val = databoxNewBytes(genval("valNEWER222", 9001 + i), 32);

            multimapAtomInsertIfNewConvert(a, &insertKey);
            multimapAtomInsertIfNewConvert(a, &val);
            flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
                &f, 2, boxes, &insertKey, &middle, true, a);

            assert(middle == flexMiddle(f, 2));

            flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
                f, 2, &key, middle, a);

            assert(found);
            databox got = {{0}};
            flexGetByTypeWithReference(found, &got, a);
            assert(databoxEqual(&key, &got));

            databox got2 = {{0}};
            bool getNext = flexGetNextByType(f, &found, &got2);
            assert(getNext);
            assert(databoxEqual(&val, &got2));
        }

        assert(flexCount(f) == (2 * 19));

        flexRepr(f);

        multimapAtomRepr(a);
        multimapAtomFree(a);
        printf("\n");
    }

#if defined(FLEX_DEBUG_EXTENSIVE)
#define GROWMAX 2200
#elif defined(NDEBUG) /* if not debugging (e.g. release build), test more */
#define GROWMAX 1200
#else /* else, this is a -O0 build and is slow */
#define GROWMAX 300
#endif

    /* Yes, this is big and ugly and non-abstracted, but it helps when needing
     * to adjust parameters or features during debugging or speed testing. */
    /* Alternate even/odd lengths to test for even or odd midpoint discovery
     * off-by-one errors */
    for (int grow = 64; grow < GROWMAX;
         grow = (grow * 2) + (grow % 2 == 0 ? 1 : 0)) {
        uint64_t tstart;
        uint64_t tend;

        /* GENERATE COMMON DATA */
        const size_t totalVals = grow;
        cleanerName(zfree, Ints) int *vals = zcalloc(totalVals, sizeof(*vals));
        for (size_t i = 0; i < totalVals; i++) {
            vals[i] = rand();
        }

        for (int sorted = 1; sorted >= 0; sorted--) {
            printf("Test non-sorted vs. sorted insert/find (%s; %d entries):\n",
                   sorted ? "SORTED" : "NON-SORTED", grow);
            {
                /* INSERT */
                if (sorted) {
                    /* ALLOCATE COMPARISON FLEXS WITH GROWTH HINT */
                    flex *fs[2];
                    fs[0] = createFlexWithGrowthHint(grow * sizeof(int) * 2);
                    fs[1] = createFlexWithGrowthHint(grow * sizeof(int) * 2);
                    for (int withMiddle = 0; withMiddle < 2; withMiddle++) {
                        flexEntry *insertMiddle = NULL;
                        tstart = timeUtilUs();
                        for (size_t i = 0; i < totalVals; i++) {
                            databox valbox = databoxNewSigned(vals[i]);
                            if (withMiddle) {
                                flexInsertByTypeSortedWithMiddle(
                                    &fs[withMiddle], &valbox, &insertMiddle);
                            } else {
                                flexInsertByTypeSorted(&fs[withMiddle],
                                                       &valbox);
                            }
                        }
                        tend = timeUtilUs();
                        uint64_t searched = tend - tstart;
                        printf("SUCCESS INSERT (%f sec total; %f us per avg "
                               "insert; %zu bytes%s)\n",
                               ((double)(searched) / 1e6),
                               ((double)(searched)) / totalVals,
                               flexBytes(fs[0]),
                               withMiddle ? " (WITH MIDDLE)" : "");
                    }

                    /* Verify both methods of creation generated exactly the
                     * same sorted flex. */
                    assert(flexBytes(fs[0]) == flexBytes(fs[1]));
                    assert(!memcmp(fs[0], fs[1], flexBytes(fs[0])));
                    f = fs[0];
                    zfree(fs[1]);
                } else {
                    /* ALLOCATE FLEX WITH GROWTH HINT */
                    f = createFlexWithGrowthHint(grow * 4);

                    tstart = timeUtilUs();
                    for (size_t i = 0; i < totalVals; i++) {
                        databox valbox = databoxNewSigned(vals[i]);
                        flexPushByType(&f, &valbox, FLEX_ENDPOINT_TAIL);
                    }

                    tend = timeUtilUs();
                    uint64_t searched = tend - tstart;
                    printf("SUCCESS INSERT (%f sec total; %f us per avg "
                           "insert; %zu bytes)\n",
                           ((double)(searched) / 1e6),
                           ((double)(searched)) / totalVals, flexBytes(f));
                }

                /* LOOKUP: 64 random finds per value hit every value many
                 * times over; the timings printed are informational */
                size_t looper = totalVals * 64;
                flexEntry *middleEntry = flexIndex(f, totalVals / 2);

                if (sorted) {
                    for (int withMiddle = 0; withMiddle < 2; withMiddle++) {
                        if (withMiddle) {
                            tstart = timeUtilUs();
                            for (size_t i = 0; i < looper; i++) {
                                databox valbox =
                                    databoxNewSigned(vals[rand() % totalVals]);
                                flexEntry *found =
                                    flexFindByTypeSortedWithMiddle(
                                        f, 1, &valbox, middleEntry);
                                assert(found);
                                databox got = {{0}};
                                flexGetByType(found, &got);
                                assert(databoxEqual(&valbox, &got));
                            }

                            tend = timeUtilUs();
                        } else {
                            tstart = timeUtilUs();
                            for (size_t i = 0; i < looper; i++) {
                                databox valbox =
                                    databoxNewSigned(vals[rand() % totalVals]);
                                flexEntry *found =
                                    flexFindByTypeSorted(f, 1, &valbox);
                                assert(found);
                                databox got = {{0}};
                                flexGetByType(found, &got);
                                assert(databoxEqual(&valbox, &got));
                            }

                            tend = timeUtilUs();
                        }

                        uint64_t searched = tend - tstart;
                        printf("SUCCESS FIND (%f sec total; %f us per avg "
                               "lookup%s)\n",
                               ((double)(searched) / 1e6),
                               ((double)(searched)) / looper,
                               withMiddle ? " (WITH MIDDLE)" : "");
                    }

                    /* Quick in-line test for splitting... */
                    cleaner(flexFree) flex *split = flexDuplicate(f);
                    flex *secondHalf = flexSplit(&split, 1);

                    flexFree(secondHalf);

                    /* DELETE SORTED ENTRIES; VERIFY DELETE WORKS */
                    for (size_t i = 0; i < totalVals; i++) {
                        databox valbox = databoxNewSigned(vals[i]);
                        flexEntry *found = flexFindByTypeSortedWithMiddle(
                            f, 1, &valbox, middleEntry);
                        assert(found);
                        flexDeleteSortedValueWithMiddle(&f, 1, found,
                                                        &middleEntry);
                        assert(middleEntry == flexMiddle(f, 1));
                        found = flexFindByTypeSortedWithMiddle(f, 1, &valbox,
                                                               middleEntry);
                        assert(middleEntry == flexMiddle(f, 1));
                        if (found) {
                            databox got = {{0}};
                            flexGetByType(found, &got);
                            assert(!found);
                        }
                    }
                } else {
                    tstart = timeUtilUs();
                    for (size_t i = 0; i < looper; i++) {
                        databox valbox =
                            databoxNewSigned(vals[rand() % totalVals]);
                        flexEntry *found = flexFindByTypeHead(f, &valbox, 0);
                        assert(found);
                        databox got = {{0}};
                        flexGetByType(found, &got);
                        assert(databoxEqual(&valbox, &got));
                    }

                    tend = timeUtilUs();
                    uint64_t searched = tend - tstart;
                    printf(
                        "SUCCESS FIND (%f sec total; %f us per avg lookup)\n",
                        ((double)(searched) / 1e6),
                        ((double)(searched)) / looper);
                }

                printf("\n");

                flexFree(f);
            }
        }
        printf("\n");
    }

    printf("Test replacing elements on insert:\n");
    {
        f = createList();
        flex *f2 = createList();

        printf("Initial list...\n");
        flexRepr(f);

        static const char *attempt = "abcdefhij";
        flexReplaceBytes(&f, flexHead(f), attempt, strlen(attempt));
        printf("Replaced head...\n");
        flexRepr(f);

        static const char *attempt2 = "zabooooooooooooo";
        flexReplaceBytes(&f, flexTail(f), attempt2, strlen(attempt2));
        printf("Replaced tail...\n");
        flexRepr(f);

        static const char *attempt3 =
            "William Shakespeare was an English poet, playwright, and actor, "
            "widely regarded as the greatest writer in the English language "
            "and the world's pre-eminent dramatist. He is often called "
            "England's national poet, and the \"Bard of Avon\". Wikipedia";
        flexReplaceBytes(&f, flexPrev(f, flexTail(f)), attempt3,
                         strlen(attempt3));
        printf("Replaced previous to tail...\n");
        flexRepr(f);

        flexReplaceBytes(&f, flexTail(f), attempt, strlen(attempt));
        printf("Replaced tail...\n");
        flexRepr(f);

        flexReplaceBytes(&f, flexPrev(f, flexTail(f)), attempt2,
                         strlen(attempt2));
        printf("Replaced previous to tail...\n");
        flexRepr(f);

        printf("SUCCESS\n\n");
        flexFree(f);
        flexFree(f2);
    }

    printf("Compare multi-insert ending in NULL:\n");
    {
        f = flexNew();
#define mku(u_) databoxNewUnsigned(u_)
        databox clusterId = mku(100);
        databox nodeId = mku(200);
        databox runId = mku(300);
        databox term = mku(400);
        databox previousIndex = mku(500);
        databox previousTerm = mku(0);
        databox leaderCommit = mku(600);
        databox timestamp = mku(700);
        databox rpcCmd = mku(800);

        databox entry_ = {
            .data.bytes.start = NULL, .len = 0, .type = DATABOX_BYTES};

        const databox *fields[] = {
            &clusterId,    &nodeId,       &runId,     &term,   &previousIndex,
            &previousTerm, &leaderCommit, &timestamp, &rpcCmd, &entry_};

        flexAppendMultipleExact(&f, 10, fields);

        flexRepr(f);
        flexFree(f);
    }

    printf("Create list of data and true/false/null:\n");
    {
        f = createList();
        const databox t = databoxBool(true);
        const databox F = databoxBool(false);
        const databox n = databoxNull();
        flexPushByType(&f, &t, FLEX_ENDPOINT_TAIL);
        flexRepr(f);

        flexPushByType(&f, &F, FLEX_ENDPOINT_TAIL);
        flexRepr(f);

        flexPushByType(&f, &n, FLEX_ENDPOINT_TAIL);
        flexRepr(f);

        flexPushByType(&f, &t, FLEX_ENDPOINT_HEAD);
        flexRepr(f);

        flexPushByType(&f, &F, FLEX_ENDPOINT_HEAD);
        flexRepr(f);

        flexPushByType(&f, &n, FLEX_ENDPOINT_HEAD);
        flexRepr(f);

        /* Random cleanups */
        for (int i = 0; i < 300; i++) {
            cleaner(flexFree) flex *f2 = flexDuplicate(f);
            cleaner(flexFree) flex *f3 = flexDuplicate(f);
            cleaner(flexFree) flex *f4 = flexDuplicate(f);

            while (flexCount(f2)) {
                int32_t idx = rand() % flexCount(f2);
                flexDeleteOffsetCountDrain(&f2, idx, 1);
            }

            while (flexCount(f3)) {
                int32_t idx = rand() % flexCount(f3);
                flexDeleteOffsetCountDrain(&f3, idx, 1);
            }

            while (flexCount(f4)) {
                int32_t idx = rand() % flexCount(f4);
                flexDeleteOffsetCountDrain(&f4, idx, 1);
            }
        }

        while (flexCount(f)) {
            int32_t idx = rand() % flexCount(f);
            flexDeleteOffsetCountDrain(&f, idx, 1);
        }

        flexFree(f);
    }

    printf("Compare strings with flex entries:\n");
    {
        f = createList();
        fe = flexIndex(f, 0);
        assert(fe == flexHead(f));
        if (!flexCompareBytes(fe, "hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "hella", 5)) {
            printf("ERROR: \"hella\"\n");
            assert(NULL);
        }

        fe = flexIndex(f, 3);
        if (!flexCompareBytes(fe, "1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "1025", 4)) {
            printf("ERROR: \"1025\"\n");
            assert(NULL);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Merge test:\n");
    {
        /* create list gives us: [hello, foo, quux, 1024] */
        f = createList();
        flex *f2 = createList();

        /* flexMerge absorbs one input into the survivor it returns, so
         * f3's bytes end up owned by the merged flex below. */
        /* @own: container; free=flexFree */
        flex *f3 = flexNew();
        cleaner(flexFree) flex *f4 = flexNew();

        if (flexMerge(&f4, &f4)) {
            printf("ERROR: Allowed merging of one flex into itself.\n");
            assert(NULL);
        }

        /* Merge two empty flexes, get empty result back. */
        f4 = flexMerge(&f3, &f4);
        flexRepr(f4);
        if (flexCount(f4)) {
            printf("ERROR: Merging two empty flexes created entries.\n");
            assert(NULL);
        }

        f2 = flexMerge(&f, &f2);
        /* merge gives us: [hello, foo, quux, 1024, hello, foo, quux, 1024] */
        flexRepr(f2);

        if (flexCount(f2) != 8) {
            printf("ERROR: Merged length not 8, but: %zu\n", flexCount(f2));
            assert(NULL);
        }

        fe = flexIndex(f2, 0);
        if (!flexCompareBytes(fe, "hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "hella", 5)) {
            printf("ERROR: \"hella\"\n");
            assert(NULL);
        }

        fe = flexIndex(f2, 3);
        if (!flexCompareBytes(fe, "1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "1025", 4)) {
            printf("ERROR: \"1025\"\n");
            assert(NULL);
        }

        fe = flexIndex(f2, 4);
        if (!flexCompareBytes(fe, "hello", 5)) {
            printf("ERROR: not \"hello\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "hella", 5)) {
            printf("ERROR: \"hella\"\n");
            assert(NULL);
        }

        fe = flexIndex(f2, 7);
        if (!flexCompareBytes(fe, "1024", 4)) {
            printf("ERROR: not \"1024\"\n");
            assert(NULL);
        }

        if (flexCompareBytes(fe, "1025", 4)) {
            printf("ERROR: \"1025\"\n");
            assert(NULL);
        }

        /* Merge a merged flex into itself... */
        cleaner(flexFree) flex *f22 = flexDuplicate(f2);
        f22 = flexMerge(&f22, &f2);
        flexRepr(f22);

        printf("SUCCESS\n\n");
    }

    printf("Test merge and place large entries:\n");
    {
        f = createList();
        flex *f2 = createList();

        f = flexMerge(&f, &f2);
        cleaner(flexFree) flex *orig = flexDuplicate(f);

        /* 24 MB: past VARINT_SPLIT_FULL_NO_ZERO_MAX_22 + 2^24, so each
         * entry's byte length takes a 5-byte encoding */
        size_t biglen = 24 * 1024 * 1024;
        cleanerName(zfree, Bytes) uint8_t *big = zmalloc(biglen);
        randbytes(big, biglen, biglen);
        databox box = databoxNewBytes(big, biglen);

        char smaller[42] = {7};
        size_t smalllen = sizeof(smaller);
        databox smallbox = databoxNewBytes(smaller, smalllen);

        flexRepr(f);
        printf("Inserting and deleting large elements in random spots:\n");
        for (int32_t i = 0; i < 8; i++) {
            if (i > 0) {
                printf(".");
                fflush(stdout);
            }

            /* Add one giant element in the middle */
            uint8_t position = rand() % flexCount(f);

            fe = flexIndex(f, position);
            flexInsertByType(&f, fe, &box);

            fe = flexIndex(f, position);
            flexInsertByType(&f, fe, &box);

            fe = flexIndex(f, position);
            flexInsertByType(&f, fe, &smallbox);

            fe = flexIndex(f, position);
            flexInsertByType(&f, fe, &box);
            if (i == 0) {
                flexRepr(f);
            }

            /* Delete giant, check all prevlens were updated correctly. */
            fe = flexIndex(f, position);
            flexDelete(&f, &fe);
            flexDelete(&f, &fe);
            flexDelete(&f, &fe);
            flexDelete(&f, &fe);
            if (i == 0) {
                flexRepr(f);
            }
        }
        printf("\n");
        flexRepr(f);

        assert(!memcmp(orig, f, flexTotalBytes_(f)));

        flexFree(f);
        f = NULL;
        printf("SUCCESS\n\n");
    }

    printf("Test rotating 4 element list 300 times:\n");
    {
        f = createList();
        for (int i = 0; i < 300; i++) {
            databox box = {{0}};
            flexGetByType(flexIndex(f, -1), &box);
            flexPushByType(&f, &box, FLEX_ENDPOINT_HEAD);
            fe = flexIndex(f, -1);
            flexDelete(&f, &fe);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test replacing element with bigger version of itself:\n");
    {
        f = flexNew();
        uint8_t thing[2048] = {0};
        memset(thing, 3, sizeof(thing));

        databox box = databoxNewBytes(thing, 24);
        flexPushByType(&f, &box, FLEX_ENDPOINT_HEAD);
        size_t expectedSize = 1 + 1 + 1 + 24 + 1;
        if (flexBytes(f) != expectedSize) {
            printf("Expected size %zu, but got %zu instead!\n", expectedSize,
                   flexBytes(f));
            assert(NULL);
        }

        fe = flexHead(f);
        flexResizeEntry(&f, fe, 2048);
        expectedSize = 2 + 1 + 2 + 2048 + 2;
        if (flexBytes(f) != expectedSize) {
            printf("Expected size %zu, but got %zu instead!\n", expectedSize,
                   flexBytes(f));
            assert(NULL);
        }

        fe = flexHead(f);
        flexGetByType(fe, &box);

        memcpy(box.data.bytes.start, thing, 2048);

        fe = flexHead(f);
        flexGetByType(fe, &box);
        assert(!memcmp(box.data.bytes.start, thing, 2048));

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test replacing element with smaller version of itself:\n");
    {
        f = flexNew();
        uint8_t thing[2048] = {0};
        memset(thing, 3, sizeof(thing));

        databox box = databoxNewBytes(thing, 2048);
        flexPushByType(&f, &box, FLEX_ENDPOINT_HEAD);
        size_t expectedSize = 2 + 1 + 2 + 2048 + 2;
        if (flexBytes(f) != expectedSize) {
            printf("Expected size %zu, but got %zu instead!\n", expectedSize,
                   flexBytes(f));
            assert(NULL);
        }

        fe = flexHead(f);
        flexResizeEntry(&f, fe, 24);
        expectedSize = 1 + 1 + 1 + 24 + 1;
        if (flexBytes(f) != expectedSize) {
            printf("Expected size %zu, but got %zu instead!\n", expectedSize,
                   flexBytes(f));
            assert(NULL);
        }

        fe = flexHead(f);
        flexGetByType(fe, &box);
        assert(!memcmp(box.data.bytes.start, thing, 24));

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test create long list and check indices:\n");
    {
        f = flexNew();
        char buf[32];
        int64_t len;

        int64_t start = 0;
#if !FLEX_TEST_SANITIZED
        int64_t loops = (1LL << 16) + 1800;
#else
        int64_t loops = (1LL << 14);
#endif
        printf("Creating big list...\n");
        for (int64_t i = start; i < loops; i++) {
            if (i % (9000) == 0) {
                printf("Populating %" PRIi64 " (%" PRIi64 " remaining)\n", i,
                       (loops - i));
            }

            len = snprintf(buf, sizeof(buf), "%" PRIi64 "", i);
            flexPushBytes(&f, buf, len, FLEX_ENDPOINT_TAIL);
        }

        printf("Validating big list...\n");
        uint64_t tstart = timeUtilUs();

        /* Every value, walking each way */
        int64_t walked = 0;
        for (fe = flexHead(f); fe; fe = flexNext(f, fe)) {
            assert(flexGet(fe, NULL, NULL, &value));
            assert(walked == value);
            walked++;
        }

        assert(walked == loops);
        for (fe = flexTail(f); fe; fe = flexPrev(f, fe)) {
            walked--;
            assert(flexGet(fe, NULL, NULL, &value));
            assert(walked == value);
        }

        assert(walked == 0);

        /* Indexing from either end (each lookup walks from the nearer
         * end): every index within 1024 of the ends or of 2^16, where the
         * values change integer width, and every 61st elsewhere */
        for (int64_t i = start; i < loops; i++) {
            const bool nearEdge = i < 1024 || loops - i <= 1024 ||
                                  (i >= 65536 - 1024 && i < 65536 + 1024);
            if (!nearEdge && i % 61 != 0) {
                continue;
            }

            fe = flexIndex(f, i);
            /* Validate head to tail */
            assert(flexGet(fe, NULL, NULL, &value));
            assert(i == value);

            /* Validate tail to head */
            fe = flexIndex(f, -i - 1);
            assert(flexGet(fe, NULL, NULL, &value));
            assert((loops - 1) - i == value);
        }

        uint64_t tend = timeUtilUs();

        printf("SUCCESS (%f sec total)\n\n", ((double)(tend - tstart) / 1e6));
        flexFree(f);
    }

    printf("Test cflex -> flex -> cflex conversions:\n");
    {
        uint8_t randBytesBuf[8192] = {0};
        size_t compressRestoreSize = 1024 * 1024;
        cleaner(zfree) void *compressRestoreBuffer =
            zcalloc(1, compressRestoreSize);
        cflex *c = (cflex *)compressRestoreBuffer;
        cleanerName(zfree, Bytes) flex *restored =
            zcalloc(1, compressRestoreSize);

        for (int iterations = 0; iterations < 64; iterations++) {
            f = flexNew();

            for (int i = 0; i < iterations; i++) {
                flexPushByType(
                    &f,
                    &DATABOX_WITH_BYTES(randBytesBuf,
                                        randbytes(randBytesBuf, 12, 128 * i)),
                    FLEX_ENDPOINT_TAIL);
            }

            const size_t fullBytes = flexBytes(f);
            const size_t fullCount = flexCount(f);

            printf("Created %zu element flex with total bytes %zu...\n",
                   fullCount, fullBytes);

            if (fullBytes < CFLEX_MINIMUM_COMPRESS_BYTES) {
                if (flexConvertToCFlex(f, compressRestoreBuffer,
                                       compressRestoreSize)) {
                    assert(NULL && "converted a too-small cflex!");
                }
                /* We didn't compress, so free and try next loop. */

                flexFree(f);
                continue;
            }

            if (fullBytes >= CFLEX_MINIMUM_COMPRESS_BYTES &&
                !flexConvertToCFlex(f, compressRestoreBuffer,
                                    compressRestoreSize)) {
                assert(NULL && "failed to convert to a cflex!");
            }

            const size_t fullBytesC = flexBytes((flex *)c);
            const size_t fullCountC = flexCount((flex *)c);
            const size_t fullCBytesC = cflexBytesCompressed(c);

            const size_t fullBytesAlloc = jebufSizeAllocation(fullBytesC);
            const size_t fullCBytesAlloc = jebufSizeAllocation(fullCBytesC);
            const bool keepCompressed =
                jebufUseNewAllocation(fullBytesC, fullCBytesC);

            printf("Compressed %zu elements with total bytes %zu down to %zu "
                   "bytes, so %s (%zu vs. %zu)...\n",
                   fullCountC, fullBytesC, fullCBytesC,
                   keepCompressed ? "KEEP COMPRESSED" : "USE UNCOMPRESSED",
                   fullBytesAlloc, fullCBytesAlloc);

            /* compressed! now restore! */
            if (!cflexConvertToFlex(c, &restored, &compressRestoreSize)) {
                assert(NULL && "Failed to restore cflex!");
            }

            /* now the original flex should match exactly the restored flex. */
            assert(memcmp(f, restored, flexBytes(f)) == 0);

            flexFree(f);
        }
    }

    printf("Test cflexConvertToFlex buffer-length contract:\n");
    {
        /* Decompression trusts the caller's buffer length to skip its
         * own realloc, so pin both sides of that contract: a too-small
         * declared buffer must grow (with the length updated to the
         * real allocation), and an exactly-sized buffer must be used
         * as-is without writing past its end. */
        uint8_t randBytesBuf[8192] = {0};
        f = flexNew();
        for (int i = 0; i < 48; i++) {
            flexPushByType(&f,
                           &DATABOX_WITH_BYTES(
                               randBytesBuf, randbytes(randBytesBuf, 64, 512)),
                           FLEX_ENDPOINT_TAIL);
        }

        const size_t totalSize = flexBytes(f);
        assert(totalSize >= CFLEX_MINIMUM_COMPRESS_BYTES);

        size_t cSize = jebufSizeAllocation(totalSize);
        cleaner(zfree) void *cBuf = zcalloc(1, cSize);
        cflex *c = (cflex *)cBuf;
        if (!flexConvertToCFlex(f, cBuf, cSize)) {
            assert(NULL && "failed to compress contract-test flex!");
        }

        /* (a) undersized declared buffer: must realloc and report the
         * new real allocation back through the length */
        size_t smallLen = 16;
        cleanerName(zfree, Bytes) flex *small = zcalloc(1, smallLen);
        if (!cflexConvertToFlex(c, &small, &smallLen)) {
            assert(NULL && "undersized restore failed!");
        }
        assert(smallLen >= totalSize);
        assert(memcmp(f, small, totalSize) == 0);

        /* (b) exactly-sized buffer: no growth needed; any write past
         * the end is caught by the sanitizer allocator */
        size_t exactLen = totalSize;
        cleanerName(zfree, Bytes) flex *exact = zcalloc(1, exactLen);
        if (!cflexConvertToFlex(c, &exact, &exactLen)) {
            assert(NULL && "exact-size restore failed!");
        }
        assert(exactLen == totalSize);
        assert(memcmp(f, exact, totalSize) == 0);

        flexFree(f);
        printf("SUCCESS\n\n");
    }

    /* Each entry is three bytes.  Both sizes take the count past 67823 to
     * its 4-byte width and back; 2 M (about 6 MB) also takes the byte
     * count to its 4-byte width.  The sanitized size is smaller because
     * every exact-size append reallocates there.  The byte count's 5-byte
     * width has its own boundary test below. */
#if !FLEX_TEST_SANITIZED
    printf("Test create flex in single byte increments up to 2 M entries:\n");
#else
    printf("Test create flex in single byte increments up to 72 k "
           "entries:\n");
#endif
    {
        f = flexNew();

#if !FLEX_TEST_SANITIZED
        const int32_t contentsSize = 1024 * 1024 * 2; /* 2 M */
#else
        const int32_t contentsSize = 1024 * 72; /* 72 k */
#endif

        int32_t fill = contentsSize;
        printf("Appending entries...\n");
        while (fill--) {
            if (fill % (1024 * 8) == 0) {
                printf(".");
                fflush(stdout);
            }

            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
        }

        printf("\n\n");
        printf("Total bytes allocated for flex: %zu\n", flexBytes(f));

        printf(
            "Deleting entries individually (randomly from Head or Tail)...\n");
        fill = 0;
        int32_t heads = 0;
        int32_t tails = 0;
        int32_t deleter = 0;
        while (flexCount(f) > 0) {
            if (deleter > contentsSize) {
                assert(NULL);
            }

            /* Randomly delete from head or tail with a bias towards tail */
            int32_t deleteAt = rand() % 128 == 0 ? 0 : -1;
            deleteAt == 0 ? heads++ : tails++;
            if (fill++ % (1024 * 8) == 0) {
                /* this only prints sample of our deletion attempts... */
                deleteAt == 0 ? printf("H") : printf("T");
                fflush(stdout);
            }

            flexDeleteOffsetCountDrain(&f, deleteAt, 1);
            deleter++;
        }

        printf("\n\n");

        printf("Deleted from head: %d\n", heads);
        printf("Deleted from tail: %d\n", tails);
        assert(heads + tails == fill);
        assert(fill == contentsSize);
        assert(flexCount(f) == 0);
        assert(flexBytes(f) == FLEX_EMPTY_HEADER_SIZE);

        flexFree(f);
        printf("SUCCESS\n\n");
    }

    printf("Test header byte count crossing its 5-byte width one entry at a "
           "time:\n");
    {
        /* The header's byte count is 5 bytes wide from MAX_22 + 2^24 up.
         * One padding entry brings the flex just under that; single-byte
         * pushes cross it, tail deletes cross back, and every step checks
         * the width against the total and the entries read back intact. */
        const size_t widen =
            VARINT_SPLIT_FULL_NO_ZERO_MAX_22 + ((size_t)1 << 24);
        const size_t padLen = widen - 256;
        cleanerName(zfree, Bytes) uint8_t *pad = zmalloc(padLen);
        for (size_t i = 0; i < padLen; i++) {
            pad[i] = (uint8_t)(i * 31 + 7);
        }

        f = flexNew();
        flexPushBytes(&f, pad, padLen, FLEX_ENDPOINT_TAIL);
        assert(FLEX_TOTAL_BYTES_WIDTH(f) == 4);

        int32_t pushed = 0;
        while (FLEX_TOTAL_BYTES_WIDTH(f) == 4) {
            assert(flexBytes(f) < widen);
            assert(pushed < 1024);
            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
            pushed++;
        }

        /* cross well past the boundary */
        for (int32_t i = 0; i < 64; i++) {
            flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);
            pushed++;
        }

        assert(FLEX_TOTAL_BYTES_WIDTH(f) == 5);
        assert(flexBytes(f) >= widen);
        assert(flexCount(f) == (size_t)(1 + pushed));

        databox got = {{0}};
        flexGetByType(flexHead(f), &got);
        assert(got.len == padLen);
        assert(memcmp(got.data.bytes.start, pad, padLen) == 0);
        for (flexEntry *walk = flexNext(f, flexHead(f)); walk;
             walk = flexNext(f, walk)) {
            flexGetByType(walk, &got);
            assert(got.len == 1 && got.data.bytes.start[0] == 'a');
        }

        int32_t deleted = 0;
        while (FLEX_TOTAL_BYTES_WIDTH(f) == 5) {
            assert(flexBytes(f) >= widen);
            flexDeleteOffsetCountDrain(&f, -1, 1);
            deleted++;
        }

        assert(FLEX_TOTAL_BYTES_WIDTH(f) == 4);
        assert(flexBytes(f) < widen);
        assert(deleted > 64);
        assert(flexCount(f) == (size_t)(1 + pushed - deleted));
        flexGetByType(flexHead(f), &got);
        assert(got.len == padLen);
        assert(memcmp(got.data.bytes.start, pad, padLen) == 0);

        /* dropping the padding from the head narrows the field to the
         * width of the few hundred bytes left */
        flexDeleteOffsetCountDrain(&f, 0, 1);
        assert(FLEX_TOTAL_BYTES_WIDTH(f) <= 2);
        assert(flexCount(f) == (size_t)(pushed - deleted));
        for (flexEntry *walk = flexHead(f); walk; walk = flexNext(f, walk)) {
            flexGetByType(walk, &got);
            assert(got.len == 1 && got.data.bytes.start[0] == 'a');
        }

        flexFree(f);
        f = NULL;
        printf("SUCCESS (%d pushes, %d deletes)\n\n", pushed, deleted);
    }
#endif

    printf("Test half float auto encoding:\n");
    {
        f = flexNew();
        float halfValue = 0.578125;
        char *halfFloatStr = "0.578125";

        databox box = databoxNewBytesString(halfFloatStr);
        flexPushByType(&f, &box, FLEX_ENDPOINT_HEAD);

        fe = flexHead(f);
        flexGetByType(fe, &box);

        if (box.type != DATABOX_FLOAT_32) {
            databoxReprSay("Expected FLOAT32, but got", &box);
            assert(NULL && "Didn't decode float!");
        }

        if (box.data.f32 != halfValue) {
            printf("Expected %f but got %f!\n", halfValue, box.data.f32);
            assert(NULL);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test IEEE float16 exact encoding:\n");
    {
        f = flexNew();
        float halfValue = 0.578125;

        flexInsertFloat16(&f, flexHead(f), halfValue);

        databox box = {{0}};
        fe = flexHead(f);
        flexGetByType(fe, &box);

        if (box.type != DATABOX_FLOAT_32) {
            assert(NULL && "Didn't decode float!");
        }

        if (box.data.f32 != halfValue) {
            printf("Expected %f but got %f!\n", halfValue, box.data.f32);
            assert(NULL);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test IEEE float16 rounded encoding:\n");
    {
        f = flexNew();
        float halfValue = -3.658203125;       /* store this */
        float halfValueStored = -3.658203125; /* read this back */

        flexInsertFloat16(&f, flexHead(f), halfValue);

        databox box = {{0}};
        fe = flexHead(f);
        flexGetByType(fe, &box);

        if (box.type != DATABOX_FLOAT_32) {
            assert(NULL && "Didn't decode float!");
        }

        if (box.data.f32 != halfValueStored) {
            printf("Expected %f but got %f!\n", halfValueStored, box.data.f32);
            assert(NULL);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    printf("Test truncated bfloat16 encoding:\n");
    {
        f = flexNew();
        float halfValue = -9992361673228288;       /* store this */
        float halfValueStored = -9992361673228288; /* read this back */

        flexInsertFloatB16(&f, flexHead(f), halfValue);

        databox box = {{0}};
        fe = flexHead(f);
        flexGetByType(fe, &box);

        if (box.type != DATABOX_FLOAT_32) {
            assert(NULL && "Didn't decode float!");
        }

        if (box.data.f32 != halfValueStored) {
            printf("Expected %f but got %f!\n", halfValueStored, box.data.f32);
            assert(NULL);
        }

        printf("SUCCESS\n\n");
        flexFree(f);
    }

    /* This test writes bytes, integers (signed/unsigned), and
     * reals (float/double) into a flex only as strings, then
     * flexInsertBytes() attempts to convert the string to integer/float
     * as appropriate. */
    for (int preallocate = 1; preallocate >= 0; preallocate--) {
        printf("Stress with random payloads of different encodings (%s):\n",
               preallocate ? "PREALLOCATED" : "REGULAR");
        {
            size_t totalAllAllocated = 0;
            size_t bufbuflen = 1 << 15;
            cleanerName(zfree, Chars) char *buf = zcalloc(1, bufbuflen);
/* If using a big memory test
 *   - or -
 * If not running under ASan */
#if !FLEX_TEST_SANITIZED
            const int numloops = 200;
#else
            /* sanitized: fewer loops: each builds up to 256 random
             * payloads of up to 32 KB, and without preallocation every
             * insert reallocates a flex of megabytes, which ASan's realloc
             * always copies */
            const int numloops = preallocate ? 50 : 8;
#endif
            const uint32_t maxTestElementCount = 256;

            int32_t seed = rand();
            printf("Seeding rand with... %d\n", seed);
            srand(seed++);
            uint64_t deletion = 0;
            uint64_t tstart = timeUtilUs();
            for (int i = 0; i < numloops; i++) {
                if (i % 5 == 0) {
                    printf(".");
                    fflush(stdout);
                }

                /* preallocate 16 MB of memory when requested */
                f = preallocate ? createFlexWithGrowthHint(1 << 24) : flexNew();

                cleaner(listRelease) list *ref = listCreate();
                listSetFreeMethod(ref, (void (*)(void *))mdscfree);
                int32_t elements = rand() % maxTestElementCount;

                /* Create lists */
                ssize_t buflen;
                ssize_t biggest = 0;
                ssize_t smallest = -1;
                for (int j = 0; j < elements; j++) {
                    flexEndpoint where =
                        (rand() & 1) ? FLEX_ENDPOINT_HEAD : FLEX_ENDPOINT_TAIL;
                    if (rand() % 2) {
                        if (rand() % 2) {
                            /* this dumb random generator is sloooooooooooow
                             */
                            buflen =
                                randstring((uint8_t *)buf, 1, bufbuflen - 1);
                        } else {
                            buflen =
                                randbytes((uint8_t *)buf, 1, bufbuflen - 1);
                        }

                        if (buflen > biggest) {
                            biggest = buflen;
                        }

                        if (buflen < smallest) {
                            smallest = buflen;
                        }
                    } else {
                        switch (rand() % 9) {
                        case 0:
                            /* unsigned tiny integer */
                            buflen = snprintf(buf, bufbuflen, "%" PRIu64,
                                              (uint64_t)(0LL + rand()) >> 20);
                            break;
                        case 1:
                            /* unsigned integer */
                            buflen = snprintf(buf, bufbuflen, "%" PRIu64,
                                              (uint64_t)(0LL + rand()));
                            break;
                        case 2:
                            /* unsigned big integer */
                            buflen = snprintf(buf, bufbuflen, "%" PRIu64,
                                              (uint64_t)(0LL + rand()) << 20);
                            break;
                        case 3:
                        case 4:
                        case 5:
                            /* signed (randomly negative) big integers.
                             * Shift unsigned: the product may be negative
                             * and the shifted value can exceed int64_t, so
                             * a signed shift is undefined.  The unsigned
                             * shift keeps the same low 64 bits. */
                            buflen = snprintf(
                                buf, bufbuflen, "%" PRId64,
                                (int64_t)((uint64_t)((int64_t)(rand() % 2 ? -1
                                                                          : 1) *
                                                     (int64_t)(0LL + rand()))
                                          << 45));
                            break;
                        case 6:
                            /* Floats */
                            buflen = StrDoubleFormatToBufNice(
                                buf, bufbuflen, (float)rand() / (float)rand());
                            break;
                        case 7:
                            /* Doubles */
                            buflen = StrDoubleFormatToBufNice(
                                buf, bufbuflen,
                                (double)((0ULL + rand()) << 45) /
                                    (double)rand());
                            break;
                        case 8:
                            /* Tiny Doubles */
                            buflen = StrDoubleFormatToBufNice(
                                buf, bufbuflen,
                                float16Decode(i < 1700 ? i + 1700 : i));
                            break;
                        default:
                            assert(NULL);
                        }
                    }

                    /* Add to flex */
                    flexPushBytes(&f, buf, buflen, where);

                    /* Add to reference list */
                    if (where == FLEX_ENDPOINT_HEAD) {
                        /* @own: container; free=mdscfree */
                        listAddNodeHead(ref, mdscnewlen(buf, buflen));
                    } else if (where == FLEX_ENDPOINT_TAIL) {
                        /* @own: container; free=mdscfree */
                        listAddNodeTail(ref, mdscnewlen(buf, buflen));
                    } else {
                        assert(NULL);
                    }
                }

                D("(%d/%d) value ranges: [%zu, %zu]\n", i, numloops, smallest,
                  biggest);

                if (!(flexCount(f) < maxTestElementCount &&
                      (size_t)elements == flexCount(f) &&
                      listLength(ref) == flexCount(f))) {
                    printf("Loopers: i = %d\n", i);
                    printf("Expected length: %d\n", elements);
                    printf("Flex count: %zu\n", flexCount(f));
                    printf("List length: %zu\n", listLength(ref));
                    flexRepr(f);
                    assert(NULL);
                }

                for (int j = 0; j < elements; j++) {
                    fe = flexIndex(f, j);
                    listNode *refnode = listIndex(ref, j);

                    assert(fe < (f + flexTotalBytes_(f)));

                    uint8_t *str = NULL;
                    ssize_t len = -1;
                    databox got = {{0}};
                    databoxWide slot;
                    flexGetByTypeInto(fe, &got, &slot);

                    if (got.type == DATABOX_BYTES) {
                        str = got.data.bytes.start;
                        len = got.len;
                        buflen = len;
                        memcpy(buf, str, buflen);
                    } else if (DATABOX_IS_NUMERIC(&got) ||
                               got.type == DATABOX_SIGNED_128 ||
                               got.type == DATABOX_UNSIGNED_128) {
                        /* randstring's digit-only strings of 20 to 39 digits
                         * are integer numerals beyond 64 bits, so they read
                         * back as 128-bit integers. */
                        buflen = flexTestSpellNumber_(&got, buf);
                    } else {
                        assert(NULL && "Unexpected type!");
                    }

                    char *refnodeval = listNodeValue(refnode);

                    if (memcmp(buf, refnodeval, buflen) != 0) {
                        for (ssize_t joo = 0; joo < buflen; joo++) {
                            if (buf[joo] != refnodeval[joo]) {
                                printf("ERROR! flex[%" PRIdPTR "] = %d vs. "
                                       "reference[%" PRIdPTR "] "
                                       "= %d\n",
                                       (ptrdiff_t)joo, buf[joo], (ptrdiff_t)joo,
                                       refnodeval[joo]);
                            }
                        }
                        ssize_t refnodelen = strlen(refnodeval);
                        printf("ERROR! flex result != reference node; %.*s "
                               "(%f, %f; %" PRIu32 ") "
                               "!= %s\n",
                               (int)buflen, (char *)buf, got.data.f32,
                               got.data.d64, (uint32_t)got.type, refnodeval);

                        if (buflen != refnodelen) {
                            printf("ERROR! flex element size != reference "
                                   "node "
                                   "size; %" PRIdPTR " != %" PRIdPTR "\n",
                                   (ptrdiff_t)buflen, (ptrdiff_t)refnodelen);
                        }

                        assert(NULL);
                    }
                }

                totalAllAllocated += flexBytes(f);

                /* random cleanup */
                uint64_t ttstart = timeUtilUs();
                while (elements--) {
                    int32_t idx = rand() % flexCount(f);
                    flexDeleteOffsetCountDrain(&f, idx, 1);
                }

                uint64_t ttend = timeUtilUs();
                deletion += ttend - ttstart;

                assert(flexTotalBytes_(f) == FLEX_EMPTY_HEADER_SIZE);
                assert(flexCount(f) == 0);

                flexFree(f);
                f = NULL;
            }

            uint64_t tend = timeUtilUs();

            printf("SUCCESS (%f sec total; %f sec deletion; %'zu total "
                   "bytes)\n\n",
                   ((double)tend - tstart) / 1e6, (double)deletion / 1e6,
                   totalAllAllocated);
        }
    }

    printf("Stress with variable flex sizes (insert + delete):\n");
    {
        /* 500 push+pop pairs every 512 entries up to 16 k, which takes
         * both header fields through their 1-, 2- and 3-byte widths: a
         * smoke-size run of what was a throughput benchmark */
        float headTotal = stress(FLEX_ENDPOINT_HEAD, 500, 16384, 512);
        float tailTotal = stress(FLEX_ENDPOINT_TAIL, 500, 16384, 512);

        printf("SUCCESS (%f sec head)\nSUCCESS (%f sec tail)\n\n",
               headTotal / 1e6, tailTotal / 1e6);
    }

    printf("Stress with variable flex sizes (insert replace):\n");
    {
        float headTotal =
            stressReplaceInline(FLEX_ENDPOINT_HEAD, 500, 16384, 512);
        float tailTotal =
            stressReplaceInline(FLEX_ENDPOINT_TAIL, 500, 16384, 512);

        printf("SUCCESS (%f sec head)\nSUCCESS (%f sec tail)\n\n",
               headTotal / 1e6, tailTotal / 1e6);
    }

    /* ================================================================
     * COMPREHENSIVE FUZZ TESTS
     * ================================================================ */

    printf("\n=== FLEX FUZZ TESTING ===\n\n");

    printf("FUZZ: integer encoding round-trip - signed boundaries: ");
    {
        f = flexNew();

        /* Test all boundary values for each integer width */
        const int64_t testVals[] = {
            /* 8-bit boundaries */
            INT8_MIN, INT8_MAX, INT8_MIN + 1, INT8_MAX - 1,
            /* 16-bit boundaries */
            INT16_MIN, INT16_MAX, INT16_MIN + 1, INT16_MAX - 1,
            /* 24-bit boundaries */
            -(1LL << 23), (1LL << 23) - 1,
            /* 32-bit boundaries */
            INT32_MIN, INT32_MAX, INT32_MIN + 1, INT32_MAX - 1,
            /* 40-bit boundaries */
            -(1LL << 39), (1LL << 39) - 1,
            /* 48-bit boundaries */
            -(1LL << 47), (1LL << 47) - 1,
            /* 56-bit boundaries */
            -(1LL << 55), (1LL << 55) - 1,
            /* 64-bit boundaries */
            INT64_MIN, INT64_MAX, INT64_MIN + 1, INT64_MAX - 1,
            /* Common values */
            -1, 0, 1, -100, 100, -1000, 1000};

        for (size_t i = 0; i < sizeof(testVals) / sizeof(testVals[0]); i++) {
            flexPushSigned(&f, testVals[i], FLEX_ENDPOINT_TAIL);
        }

        /* Verify all values can be retrieved correctly */
        fe = flexHead(f);
        for (size_t i = 0; i < sizeof(testVals) / sizeof(testVals[0]); i++) {
            databox box;
            flexGetByType(fe, &box);
            int64_t retrieved;
            if (box.type == DATABOX_SIGNED_64) {
                retrieved = box.data.i;
            } else if (box.type == DATABOX_UNSIGNED_64) {
                retrieved = (int64_t)box.data.u;
            } else {
                printf("\nFAIL: unexpected type %d for index %zu\n",
                       (int)box.type, i);
                assert(NULL);
            }
            if (retrieved != testVals[i]) {
                printf("\nFAIL: expected %" PRId64 ", got %" PRId64
                       " at index %zu\n",
                       testVals[i], retrieved, i);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: integer encoding round-trip - unsigned boundaries: ");
    {
        f = flexNew();

        const uint64_t testVals[] = {0,
                                     1,
                                     UINT8_MAX,
                                     UINT16_MAX,
                                     (1ULL << 24) - 1, /* 24-bit max */
                                     UINT32_MAX,
                                     (1ULL << 40) - 1, /* 40-bit max */
                                     (1ULL << 48) - 1, /* 48-bit max */
                                     (1ULL << 56) - 1, /* 56-bit max */
                                     UINT64_MAX,
                                     UINT64_MAX - 1};

        for (size_t i = 0; i < sizeof(testVals) / sizeof(testVals[0]); i++) {
            flexPushUnsigned(&f, testVals[i], FLEX_ENDPOINT_TAIL);
        }

        fe = flexHead(f);
        for (size_t i = 0; i < sizeof(testVals) / sizeof(testVals[0]); i++) {
            databox box;
            flexGetByType(fe, &box);
            uint64_t retrieved = (box.type == DATABOX_UNSIGNED_64)
                                     ? box.data.u
                                     : (uint64_t)box.data.i;
            if (retrieved != testVals[i]) {
                printf("\nFAIL: expected %" PRIu64 ", got %" PRIu64
                       " at index %zu\n",
                       testVals[i], retrieved, i);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: bytes encoding round-trip - various lengths: ");
    {
        f = flexNew();

        /* Test string lengths from 0 to 8192 */
        uint8_t buf[8192];
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)(i & 0xFF);
        }

        const size_t testLens[] = {0,    1,    2,    7,    8,    15,   16,
                                   31,   32,   63,   64, /* embedded boundary */
                                   127,  128,  255,  256,  511,  512,  1023,
                                   1024, 2047, 2048, 4095, 4096, 8191, 8192};

        for (size_t i = 0; i < sizeof(testLens) / sizeof(testLens[0]); i++) {
            flexPushBytes(&f, buf, testLens[i], FLEX_ENDPOINT_TAIL);
        }

        fe = flexHead(f);
        for (size_t i = 0; i < sizeof(testLens) / sizeof(testLens[0]); i++) {
            databox box;
            flexGetByType(fe, &box);
            size_t len = databoxLen(&box);
            if (len != testLens[i]) {
                printf("\nFAIL: expected len %zu, got %zu at index %zu\n",
                       testLens[i], len, i);
                assert(NULL);
            }
            if (testLens[i] > 0) {
                const uint8_t *data = (const uint8_t *)box.data.bytes.start;
                if (memcmp(data, buf, testLens[i]) != 0) {
                    printf("\nFAIL: data mismatch at index %zu\n", i);
                    assert(NULL);
                }
            }
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: float encoding round-trip: ");
    {
        f = flexNew();

        static const float floatVals[] = {0.0f,
                                          -0.0f,
                                          1.0f,
                                          -1.0f,
                                          1.175494351e-38f,
                                          3.402823466e+38f,
                                          -3.402823466e+38f,
                                          3.14159f,
                                          -2.71828f,
                                          1e-10f,
                                          1e10f};
        static const size_t floatCount = 11;

        static const double doubleVals[] = {0.0,
                                            -0.0,
                                            1.0,
                                            -1.0,
                                            2.2250738585072014e-308,
                                            1.7976931348623157e+308,
                                            -1.7976931348623157e+308,
                                            3.14159265358979,
                                            -2.71828182845904,
                                            1e-100,
                                            1e100};
        static const size_t doubleCount = 11;

        for (size_t i = 0; i < floatCount; i++) {
            flexPushFloat(&f, floatVals[i], FLEX_ENDPOINT_TAIL);
        }
        for (size_t i = 0; i < doubleCount; i++) {
            flexPushDouble(&f, doubleVals[i], FLEX_ENDPOINT_TAIL);
        }

        fe = flexHead(f);
        for (size_t i = 0; i < floatCount; i++) {
            databox box;
            flexGetByType(fe, &box);
            /* flex may convert certain float values to integers for efficiency
             */
            double retrieved = 0;
            if (box.type == DATABOX_FLOAT_32) {
                retrieved = box.data.f32;
            } else if (box.type == DATABOX_DOUBLE_64) {
                retrieved = box.data.d64;
            } else if (box.type == DATABOX_SIGNED_64) {
                retrieved = (double)box.data.i;
            } else if (box.type == DATABOX_UNSIGNED_64) {
                retrieved = (double)box.data.u;
            }
            /* Compare with tolerance for float */
            double diff = retrieved - (double)floatVals[i];
            if (diff < 0) {
                diff = -diff;
            }
            if (floatVals[i] != 0.0f && diff / (double)floatVals[i] > 1e-5) {
                printf("\nFAIL: float mismatch at index %zu: got %g, expected "
                       "%g\n",
                       i, retrieved, (double)floatVals[i]);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }
        for (size_t i = 0; i < doubleCount; i++) {
            databox box;
            flexGetByType(fe, &box);
            /* flex may convert certain double values to integers for efficiency
             */
            double retrieved = 0;
            if (box.type == DATABOX_DOUBLE_64) {
                retrieved = box.data.d64;
            } else if (box.type == DATABOX_FLOAT_32) {
                retrieved = box.data.f32;
            } else if (box.type == DATABOX_SIGNED_64) {
                retrieved = (double)box.data.i;
            } else if (box.type == DATABOX_UNSIGNED_64) {
                retrieved = (double)box.data.u;
            }
            double diff = retrieved - doubleVals[i];
            if (diff < 0) {
                diff = -diff;
            }
            if (doubleVals[i] != 0.0 && diff / doubleVals[i] > 1e-14) {
                printf("\nFAIL: double mismatch at index %zu: got %g, expected "
                       "%g\n",
                       i, retrieved, doubleVals[i]);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: sorted insert and binary search - integers: ");
    {
        f = flexNew();
        flexEntry *middle = NULL;
        const size_t count = 1000;

        /* Insert unique sequential values, track in oracle */
        cleanerName(zfree, I64) int64_t *oracle =
            zcalloc(count, sizeof(int64_t));

        srand(11111);
        for (size_t i = 0; i < count; i++) {
            /* Use unique values: multiply by prime to spread out */
            int64_t val = (int64_t)i * 7 - 3500;
            oracle[i] = val;

            databox box = databoxNewSigned(val);
            flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
        }

        /* Verify count */
        if (flexCount(f) != count) {
            printf("\nFAIL: count mismatch: flex=%zu expected=%zu\n",
                   flexCount(f), count);
            assert(NULL);
        }

        /* Verify all values can be found */
        for (size_t i = 0; i < count; i++) {
            databox box = databoxNewSigned(oracle[i]);
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 1, &box, middle);
            if (!found) {
                printf("\nFAIL: value %" PRId64 " not found!\n", oracle[i]);
                assert(NULL);
            }
        }

        /* Verify sorted order by iterating */
        int64_t prev = INT64_MIN;
        fe = flexHead(f);
        for (size_t i = 0; i < count; i++) {
            databox box;
            flexGetByType(fe, &box);
            int64_t val = (box.type == DATABOX_SIGNED_64) ? box.data.i
                                                          : (int64_t)box.data.u;
            if (val <= prev) {
                printf("\nFAIL: not sorted at %zu: %" PRId64 " <= %" PRId64
                       "\n",
                       i, val, prev);
                assert(NULL);
            }
            prev = val;
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: sorted insert and binary search - strings: ");
    {
        f = flexNew();
        flexEntry *middle = NULL;
        const size_t count = 500;

        /* Insert unique strings */
        for (size_t i = 0; i < count; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%06zu", i);

            databox box = databoxNewBytesString(buf);
            flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
        }

        /* Verify count */
        if (flexCount(f) != count) {
            printf("\nFAIL: count mismatch: flex=%zu expected=%zu\n",
                   flexCount(f), count);
            assert(NULL);
        }

        /* Verify all strings can be found */
        for (size_t i = 0; i < count; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%06zu", i);
            databox box = databoxNewBytesString(buf);
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 1, &box, middle);
            if (!found) {
                printf("\nFAIL: '%s' not found!\n", buf);
                assert(NULL);
            }
        }

        /* Verify sorted order */
        char prevBuf[32] = "";
        fe = flexHead(f);
        for (size_t i = 0; i < count; i++) {
            databox box;
            flexGetByType(fe, &box);
            const char *str = (const char *)box.data.bytes.start;
            if (strcmp(str, prevBuf) <= 0 && i > 0) {
                printf("\nFAIL: not sorted at %zu\n", i);
                assert(NULL);
            }
            snprintf(prevBuf, sizeof(prevBuf), "%.*s", (int)databoxLen(&box),
                     str);
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: random push/pop with oracle verification: ");
    {
        f = flexNew();
        const size_t maxSize = 1000;
        cleanerName(zfree, I64) int64_t *oracle =
            zcalloc(maxSize, sizeof(int64_t));
        size_t oracleCount = 0;

        srand(33333);
        size_t pushOps = 0, popOps = 0;

        for (size_t round = 0; round < 10000; round++) {
            int op = rand() % 10;

            if (op < 6 && oracleCount < maxSize) {
                /* Push (60%) */
                int64_t val = (int64_t)(rand() % 100000) - 50000;
                int where = rand() % 2;

                if (where == 0) {
                    /* Push head */
                    flexPushSigned(&f, val, FLEX_ENDPOINT_HEAD);
                    memmove(&oracle[1], &oracle[0],
                            oracleCount * sizeof(int64_t));
                    oracle[0] = val;
                } else {
                    /* Push tail */
                    flexPushSigned(&f, val, FLEX_ENDPOINT_TAIL);
                    oracle[oracleCount] = val;
                }
                oracleCount++;
                pushOps++;
            } else if (oracleCount > 0) {
                /* Pop (40%) */
                int where = rand() % 2;

                if (where == 0) {
                    /* Pop head */
                    fe = flexHead(f);
                    databox box;
                    flexGetByType(fe, &box);
                    int64_t got = (box.type == DATABOX_SIGNED_64)
                                      ? box.data.i
                                      : (int64_t)box.data.u;
                    if (got != oracle[0]) {
                        printf("\nFAIL: head mismatch: got %" PRId64
                               " expected %" PRId64 "\n",
                               got, oracle[0]);
                        assert(NULL);
                    }
                    flexDeleteHead(&f);
                    memmove(&oracle[0], &oracle[1],
                            (oracleCount - 1) * sizeof(int64_t));
                } else {
                    /* Pop tail */
                    fe = flexTail(f);
                    databox box;
                    flexGetByType(fe, &box);
                    int64_t got = (box.type == DATABOX_SIGNED_64)
                                      ? box.data.i
                                      : (int64_t)box.data.u;
                    if (got != oracle[oracleCount - 1]) {
                        printf("\nFAIL: tail mismatch: got %" PRId64
                               " expected %" PRId64 "\n",
                               got, oracle[oracleCount - 1]);
                        assert(NULL);
                    }
                    flexDeleteTail(&f);
                }
                oracleCount--;
                popOps++;
            }

            /* Periodic full verification */
            if (round % 1000 == 0) {
                if (flexCount(f) != oracleCount) {
                    printf("\nFAIL: count mismatch at round %zu\n", round);
                    assert(NULL);
                }
            }
        }

        /* Final verification */
        if (flexCount(f) != oracleCount) {
            printf("\nFAIL: final count mismatch\n");
            assert(NULL);
        }

        fe = flexHead(f);
        for (size_t i = 0; i < oracleCount; i++) {
            databox box;
            flexGetByType(fe, &box);
            int64_t got = (box.type == DATABOX_SIGNED_64) ? box.data.i
                                                          : (int64_t)box.data.u;
            if (got != oracle[i]) {
                printf("\nFAIL: final verification mismatch at %zu\n", i);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }

        flexFree(f);
        printf("push=%zu pop=%zu final=%zu... OK\n", pushOps, popOps,
               oracleCount);
    }

    printf("FUZZ: mixed type insertions: ");
    {
        f = flexNew();

        srand(44444);
        for (size_t i = 0; i < 1000; i++) {
            int type = rand() % 5;
            switch (type) {
            case 0: {
                int64_t val = (int64_t)(rand() % 100000) - 50000;
                flexPushSigned(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 1: {
                uint64_t val = rand() % 100000;
                flexPushUnsigned(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 2: {
                char buf[64];
                snprintf(buf, sizeof(buf), "str_%d", rand());
                flexPushBytes(&f, buf, strlen(buf), FLEX_ENDPOINT_TAIL);
                break;
            }
            case 3: {
                float val = (float)(rand() % 10000) / 100.0f;
                flexPushFloat(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            case 4: {
                double val = (double)(rand() % 10000) / 100.0;
                flexPushDouble(&f, val, FLEX_ENDPOINT_TAIL);
                break;
            }
            }
        }

        if (flexCount(f) != 1000) {
            printf("\nFAIL: expected 1000 elements, got %zu\n", flexCount(f));
            assert(NULL);
        }

        /* Iterate through and verify all entries are readable */
        fe = flexHead(f);
        size_t count = 0;
        while (fe) {
            databox box;
            flexGetByType(fe, &box);
            /* Just verify we can read it without crashing */
            count++;
            fe = flexNext(f, fe);
        }

        if (count != 1000) {
            printf("\nFAIL: iteration count %zu != 1000\n", count);
            assert(NULL);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: iterator forward and backward consistency: ");
    {
        f = flexNew();
        const size_t count = 500;

        for (size_t i = 0; i < count; i++) {
            flexPushSigned(&f, (int64_t)i, FLEX_ENDPOINT_TAIL);
        }

        /* Forward iteration */
        fe = flexHead(f);
        for (size_t i = 0; i < count; i++) {
            databox box;
            flexGetByType(fe, &box);
            if ((size_t)box.data.i != i) {
                printf("\nFAIL: forward iteration at %zu\n", i);
                assert(NULL);
            }
            fe = flexNext(f, fe);
        }

        /* Backward iteration */
        fe = flexTail(f);
        for (size_t i = 0; i < count; i++) {
            databox box;
            flexGetByType(fe, &box);
            if ((size_t)box.data.i != (count - 1 - i)) {
                printf("\nFAIL: backward iteration at %zu\n", i);
                assert(NULL);
            }
            fe = flexPrev(f, fe);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: delete operations with verification: ");
    {
        f = flexNew();
        const size_t count = 100;

        for (size_t i = 0; i < count; i++) {
            flexPushSigned(&f, (int64_t)i, FLEX_ENDPOINT_TAIL);
        }

        /* Delete from middle */
        fe = flexIndex(f, 50);
        flexDelete(&f, &fe);
        if (flexCount(f) != count - 1) {
            printf("\nFAIL: delete from middle failed\n");
            assert(NULL);
        }

        /* Verify element 49 is followed by 51 */
        fe = flexIndex(f, 49);
        databox box;
        flexGetByType(fe, &box);
        if (box.data.i != 49) {
            printf("\nFAIL: element before deleted is wrong\n");
            assert(NULL);
        }
        fe = flexNext(f, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 51) {
            printf("\nFAIL: element after deleted is wrong\n");
            assert(NULL);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: range delete: ");
    {
        f = flexNew();

        for (int i = 0; i < 100; i++) {
            flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
        }

        /* Delete range [20, 30) */
        flexDeleteRange(&f, 20, 10);

        if (flexCount(f) != 90) {
            printf("\nFAIL: count after range delete: %zu\n", flexCount(f));
            assert(NULL);
        }

        /* Verify 19 is followed by 30 */
        fe = flexIndex(f, 19);
        databox box;
        flexGetByType(fe, &box);
        if (box.data.i != 19) {
            printf("\nFAIL: element before range is wrong\n");
            assert(NULL);
        }
        fe = flexNext(f, fe);
        flexGetByType(fe, &box);
        if (box.data.i != 30) {
            printf("\nFAIL: element after range is wrong\n");
            assert(NULL);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: search for nonexistent keys in sorted flex: ");
    {
        f = flexNew();
        flexEntry *middle = NULL;

        /* Insert even numbers only */
        for (int i = 0; i < 100; i += 2) {
            databox box = databoxNewSigned(i);
            flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
        }

        /* Search for odd numbers - should all fail */
        for (int i = 1; i < 100; i += 2) {
            databox box = databoxNewSigned(i);
            flexEntry *found =
                flexFindByTypeSortedWithMiddle(f, 1, &box, middle);
            if (found) {
                printf("\nFAIL: found nonexistent %d\n", i);
                assert(NULL);
            }
        }

        /* Search for out-of-range values */
        databox box = databoxNewSigned(-100);
        if (flexFindByTypeSortedWithMiddle(f, 1, &box, middle)) {
            printf("\nFAIL: found -100\n");
            assert(NULL);
        }
        box = databoxNewSigned(1000);
        if (flexFindByTypeSortedWithMiddle(f, 1, &box, middle)) {
            printf("\nFAIL: found 1000\n");
            assert(NULL);
        }

        flexFree(f);
        printf("OK\n");
    }

    printf("FUZZ: stress sorted operations - 5K inserts/finds/deletes: ");
    {
        f = flexNew();
        flexEntry *middle = NULL;

        /*
         * IMPORTANT: flexInsertByTypeSortedWithMiddle ALLOWS duplicates!
         * It's a sorted list, not a set. The return value indicates if
         * the key existed before, but it still inserts.
         *
         * For set-like behavior, we must check before inserting.
         */
        const size_t keySpace = 10000;
        cleanerName(zfree, Bytes) uint8_t *exists =
            zcalloc((keySpace + 7) / 8, 1);
        size_t existCount = 0;

#define FUZZ_BIT_SET(arr, idx) ((arr)[(idx) / 8] |= (1 << ((idx) % 8)))
#define FUZZ_BIT_CLR(arr, idx) ((arr)[(idx) / 8] &= ~(1 << ((idx) % 8)))
#define FUZZ_BIT_GET(arr, idx) (((arr)[(idx) / 8] >> ((idx) % 8)) & 1)

        srand(55555);
        size_t insertOps = 0, deleteOps = 0, findOps = 0;

        for (size_t round = 0; round < 5000; round++) {
            int op = rand() % 10;
            int key = rand() % keySpace;
            databox box = databoxNewSigned(key);

            if (op < 5) {
                /* Insert (50%) - only insert if not already present */
                if (!FUZZ_BIT_GET(exists, key)) {
                    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
                    FUZZ_BIT_SET(exists, key);
                    existCount++;
                }
                insertOps++;
            } else if (op < 8) {
                /* Find (30%) */
                flexEntry *found =
                    flexFindByTypeSortedWithMiddle(f, 1, &box, middle);
                bool shouldExist = FUZZ_BIT_GET(exists, key);
                if (found && !shouldExist) {
                    printf("\nFAIL: found nonexistent %d\n", key);
                    assert(NULL);
                }
                if (!found && shouldExist) {
                    printf("\nFAIL: existing %d not found\n", key);
                    assert(NULL);
                }
                findOps++;
            } else {
                /* Delete (20%) */
                if (FUZZ_BIT_GET(exists, key)) {
                    flexEntry *found =
                        flexFindByTypeSortedWithMiddle(f, 1, &box, middle);
                    if (found) {
                        flexDeleteSortedValueWithMiddle(&f, 1, found, &middle);
                        FUZZ_BIT_CLR(exists, key);
                        existCount--;
                    } else {
                        printf("\nFAIL: key %d marked as existing but not "
                               "found for delete\n",
                               key);
                        assert(NULL);
                    }
                }
                deleteOps++;
            }

            /* Periodic verification */
            if (round % 500 == 0) {
                if (flexCount(f) != existCount) {
                    printf("\nFAIL: count mismatch at round %zu: flex=%zu "
                           "oracle=%zu\n",
                           round, flexCount(f), existCount);
                    assert(NULL);
                }
            }
        }

#undef FUZZ_BIT_SET
#undef FUZZ_BIT_CLR
#undef FUZZ_BIT_GET

        flexFree(f);
        printf("I=%zu D=%zu F=%zu final=%zu... OK\n", insertOps, deleteOps,
               findOps, existCount);
    }

    printf("\n=== All flex fuzz tests passed! ===\n\n");

    /* ================================================================
     * VARINT ENCODING BOUNDARY TESTS
     *
     * The flex encoding uses varintSplitFullNoZero for length encoding:
     * - 1 byte:  0-64 bytes (VARINT_SPLIT_FULL_NO_ZERO_MAX_6 = 64)
     * - 2 bytes: 65-16447 bytes (VARINT_SPLIT_FULL_NO_ZERO_MAX_14)
     * - 3 bytes: 16448+ bytes (VARINT_SPLIT_FULL_NO_ZERO_MAX_22)
     *
     * These tests verify correct behavior at encoding boundaries.
     * ================================================================ */

    printf("Test varint encoding boundary: 1-byte to 2-byte (64 bytes):\n");
    {
        /* Test strings at exactly the encoding boundary lengths.
         * The encoding stores the *data* length, so we test data lengths
         * of 63, 64, 65 bytes. */
        f = flexNew();

        /* Create test strings at boundary lengths */
        char str63[64]; /* 63 bytes of data + null */
        char str64[65]; /* 64 bytes of data + null */
        char str65[66]; /* 65 bytes of data + null */

        memset(str63, 'A', 63);
        str63[63] = '\0';
        memset(str64, 'B', 64);
        str64[64] = '\0';
        memset(str65, 'C', 65);
        str65[65] = '\0';

        /* Insert 63-byte string (should use 1-byte encoding) */
        flexPushBytes(&f, str63, 63, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 1);
        size_t bytes63 = flexBytes(f);

        /* Insert 64-byte string (boundary: should still use 1-byte encoding) */
        flexPushBytes(&f, str64, 64, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 2);
        size_t bytes64 = flexBytes(f);

        /* Insert 65-byte string (should use 2-byte encoding) */
        flexPushBytes(&f, str65, 65, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 3);
        size_t bytes65 = flexBytes(f);

        /* Verify all strings can be read back correctly */
        flexEntry *fe63 = flexIndex(f, 0);
        flexEntry *fe64 = flexIndex(f, 1);
        flexEntry *fe65 = flexIndex(f, 2);

        databox box63 = {{0}};
        databox box64 = {{0}};
        databox box65 = {{0}};
        flexGetByType(fe63, &box63);
        flexGetByType(fe64, &box64);
        flexGetByType(fe65, &box65);

        assert(box63.len == 63);
        assert(memcmp(box63.data.bytes.start, str63, 63) == 0);
        assert(box64.len == 64);
        assert(memcmp(box64.data.bytes.start, str64, 64) == 0);
        assert(box65.len == 65);
        assert(memcmp(box65.data.bytes.start, str65, 65) == 0);

        /* The 65-byte entry uses 2-byte encoding vs 1-byte for 64-byte.
         * So we expect (bytes65 - bytes64) > (bytes64 - bytes63) due to
         * the extra encoding byte on both forward and backward headers. */
        printf("  Sizes: 63B entry in %zu bytes, 64B in %zu (+%zu), "
               "65B in %zu (+%zu)\n",
               bytes63, bytes64, bytes64 - bytes63, bytes65, bytes65 - bytes64);

        /* Delete middle element and verify structure integrity */
        flexEntry *toDelete = flexIndex(f, 1);
        flexDelete(&f, &toDelete);
        assert(flexCount(f) == 2);

        /* Verify remaining elements */
        fe63 = flexIndex(f, 0);
        fe65 = flexIndex(f, 1);
        flexGetByType(fe63, &box63);
        flexGetByType(fe65, &box65);
        assert(box63.len == 63);
        assert(box65.len == 65);

        flexFree(f);
        printf("  OK\n");
    }

    printf("Test varint encoding boundary: 2-byte to 3-byte (16447 bytes):\n");
    {
        /* The 2-byte→3-byte boundary is at 16447 bytes.
         * Testing exact boundary: 16446, 16447, 16448 bytes. */
        f = flexNew();

        /* Create large test strings at boundary lengths */
        cleanerName(zfree, Chars) char *str16446 = zmalloc(16447);
        cleanerName(zfree, Chars) char *str16447 = zmalloc(16448);
        cleanerName(zfree, Chars) char *str16448 = zmalloc(16449);

        memset(str16446, 'X', 16446);
        str16446[16446] = '\0';
        memset(str16447, 'Y', 16447);
        str16447[16447] = '\0';
        memset(str16448, 'Z', 16448);
        str16448[16448] = '\0';

        /* Insert strings at each boundary */
        flexPushBytes(&f, str16446, 16446, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 1);
        size_t bytes16446 = flexBytes(f);

        flexPushBytes(&f, str16447, 16447, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 2);
        size_t bytes16447 = flexBytes(f);

        flexPushBytes(&f, str16448, 16448, FLEX_ENDPOINT_TAIL);
        assert(flexCount(f) == 3);
        size_t bytes16448 = flexBytes(f);

        /* Verify all strings can be read back correctly */
        flexEntry *fe16446 = flexIndex(f, 0);
        flexEntry *fe16447 = flexIndex(f, 1);
        flexEntry *fe16448 = flexIndex(f, 2);

        databox box16446 = {{0}};
        databox box16447 = {{0}};
        databox box16448 = {{0}};
        flexGetByType(fe16446, &box16446);
        flexGetByType(fe16447, &box16447);
        flexGetByType(fe16448, &box16448);

        assert(box16446.len == 16446);
        assert(memcmp(box16446.data.bytes.start, str16446, 16446) == 0);
        assert(box16447.len == 16447);
        assert(memcmp(box16447.data.bytes.start, str16447, 16447) == 0);
        assert(box16448.len == 16448);
        assert(memcmp(box16448.data.bytes.start, str16448, 16448) == 0);

        printf("  Sizes: 16446B entry total %zu, 16447B total %zu (+%zu), "
               "16448B total %zu (+%zu)\n",
               bytes16446, bytes16447, bytes16447 - bytes16446, bytes16448,
               bytes16448 - bytes16447);

        /* Test iteration across large entries */
        flexEntry *iter = flexHead(f);
        int count = 0;
        while (iter) {
            databox box = {{0}};
            flexGetByType(iter, &box);
            if (count == 0) {
                assert(box.len == 16446);
            } else if (count == 1) {
                assert(box.len == 16447);
            } else if (count == 2) {
                assert(box.len == 16448);
            }
            iter = flexNext(f, iter);
            count++;
        }
        assert(count == 3);

        /* Test reverse iteration */
        iter = flexTail(f);
        count = 0;
        while (iter) {
            databox box = {{0}};
            flexGetByType(iter, &box);
            if (count == 0) {
                assert(box.len == 16448);
            } else if (count == 1) {
                assert(box.len == 16447);
            } else if (count == 2) {
                assert(box.len == 16446);
            }
            iter = flexPrev(f, iter);
            count++;
        }
        assert(count == 3);

        /* Delete and re-insert to test encoding transitions */
        flexEntry *toDelete = flexIndex(f, 1); /* Delete 16447 */
        flexDelete(&f, &toDelete);
        assert(flexCount(f) == 2);

        /* Re-insert at head */
        flexPushBytes(&f, str16447, 16447, FLEX_ENDPOINT_HEAD);
        assert(flexCount(f) == 3);

        /* Verify order: 16447, 16446, 16448 */
        databox b0 = {{0}}, b1 = {{0}}, b2 = {{0}};
        flexGetByType(flexIndex(f, 0), &b0);
        flexGetByType(flexIndex(f, 1), &b1);
        flexGetByType(flexIndex(f, 2), &b2);
        assert(b0.len == 16447);
        assert(b1.len == 16446);
        assert(b2.len == 16448);

        flexFree(f);
        printf("  OK\n");
    }

    printf("Test encoding boundary transitions with in-place replacement:\n");
    {
        /* Test replacing entries near encoding boundaries to verify
         * the flex correctly handles encoding size changes. */
        f = flexNew();

        /* Start with a 64-byte string (1-byte encoding) */
        char str64[65];
        memset(str64, 'A', 64);
        str64[64] = '\0';
        flexPushBytes(&f, str64, 64, FLEX_ENDPOINT_TAIL);

        /* Also add a small marker entry */
        flexPushSigned(&f, 12345, FLEX_ENDPOINT_TAIL);

        size_t origBytes = flexBytes(f);
        flexEntry *feReplace = flexIndex(f, 0);

        /* Replace with 65-byte string (forces 2-byte encoding) */
        char str65[66];
        memset(str65, 'B', 65);
        str65[65] = '\0';
        databox replacement = databoxNewBytes(str65, 65);
        flexReplaceByType(&f, feReplace, &replacement);

        size_t newBytes = flexBytes(f);
        assert(newBytes > origBytes); /* Should grow due to encoding change */

        /* Verify replacement worked */
        databox got = {{0}};
        flexGetByType(flexIndex(f, 0), &got);
        assert(got.len == 65);
        assert(memcmp(got.data.bytes.start, str65, 65) == 0);

        /* Verify second entry wasn't corrupted */
        databox marker = {{0}};
        flexGetByType(flexIndex(f, 1), &marker);
        assert(marker.data.i == 12345);

        /* Now shrink back to 63 bytes (back to 1-byte encoding) */
        char str63[64];
        memset(str63, 'C', 63);
        str63[63] = '\0';
        databox shrink = databoxNewBytes(str63, 63);
        fe = flexIndex(f, 0);
        flexReplaceByType(&f, fe, &shrink);

        /* Verify shrink worked */
        flexGetByType(flexIndex(f, 0), &got);
        assert(got.len == 63);
        assert(memcmp(got.data.bytes.start, str63, 63) == 0);

        /* Marker still valid */
        flexGetByType(flexIndex(f, 1), &marker);
        assert(marker.data.i == 12345);

        flexFree(f);
        printf("  OK\n");
    }

    printf("Test sorted operations at encoding boundaries:\n");
    {
        /* Test sorted insert/find/delete with entries near encoding
         * boundaries to ensure the comparison and middle-tracking
         * logic handles variable-width encodings correctly. */
        f = flexNew();
        flexEntry *middle = NULL;

        /* Insert strings that sort around encoding boundaries */
        char key63[64], key64[65], key65[66];
        memset(key63, 'M', 63);
        key63[63] = '\0';
        memset(key64, 'N', 64);
        key64[64] = '\0';
        memset(key65, 'O', 65);
        key65[65] = '\0';

        /* Insert in non-sorted order to test insertion logic */
        databox box64 = databoxNewBytes(key64, 64);
        databox val64 = databoxNewSigned(64);
        const databox *elems64[2] = {&box64, &val64};
        flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, elems64, &middle);

        databox box63 = databoxNewBytes(key63, 63);
        databox val63 = databoxNewSigned(63);
        const databox *elems63[2] = {&box63, &val63};
        flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, elems63, &middle);

        databox box65 = databoxNewBytes(key65, 65);
        databox val65 = databoxNewSigned(65);
        const databox *elems65[2] = {&box65, &val65};
        flexInsertByTypeSortedWithMiddleMultiDirect(&f, 2, elems65, &middle);

        assert(flexCount(f) == 6); /* 3 key-value pairs */

        /* Find each key and verify value */
        flexEntry *found63 =
            flexFindByTypeSortedWithMiddle(f, 2, &box63, middle);
        flexEntry *found64 =
            flexFindByTypeSortedWithMiddle(f, 2, &box64, middle);
        flexEntry *found65 =
            flexFindByTypeSortedWithMiddle(f, 2, &box65, middle);

        assert(found63 != NULL);
        assert(found64 != NULL);
        assert(found65 != NULL);

        databox gotVal = {{0}};
        flexEntry *valEntry = flexNext(f, found63);
        flexGetByType(valEntry, &gotVal);
        assert(gotVal.data.i == 63);

        valEntry = flexNext(f, found64);
        flexGetByType(valEntry, &gotVal);
        assert(gotVal.data.i == 64);

        valEntry = flexNext(f, found65);
        flexGetByType(valEntry, &gotVal);
        assert(gotVal.data.i == 65);

        /* Delete middle entry and verify structure */
        flexDeleteSortedValueWithMiddle(&f, 2, found64, &middle);
        assert(flexCount(f) == 4);

        /* Verify remaining entries */
        found63 = flexFindByTypeSortedWithMiddle(f, 2, &box63, middle);
        found64 = flexFindByTypeSortedWithMiddle(f, 2, &box64, middle);
        found65 = flexFindByTypeSortedWithMiddle(f, 2, &box65, middle);

        assert(found63 != NULL);
        assert(found64 == NULL); /* Should be deleted */
        assert(found65 != NULL);

        flexFree(f);
        f = NULL;
        printf("  OK\n");
    }

    printf("\n=== All varint encoding boundary tests passed! ===\n\n");

    /* ================================================================
     * FLEX VALIDATE BUFFER TESTS
     *
     * flexValidateBuffer() is the O(1) envelope check used by
     * persistRestore paths that consume flex bytes from untrusted
     * sources (WAL replay, cross-node replication, on-disk recovery).
     * The function must:
     *   - reject NULL / undersize input without dereferencing
     *   - reject buffers whose leading varint claims a width that
     *     would read past the end of the input
     *   - reject buffers whose self-described total-bytes != buflen
     *   - accept a freshly-built valid flex
     *   - accept after the buffer grows past the 1B→2B header
     *     varint boundary
     * ================================================================ */

    printf("Test flexValidateBuffer rejects NULL / undersize input:\n");
    {
        assert(flexValidateBuffer(NULL, 0) == false);
        assert(flexValidateBuffer(NULL, 1024) == false);
        const uint8_t empty[1] = {0};
        assert(flexValidateBuffer(empty, 0) == false);
        assert(flexValidateBuffer(empty, 1) == false);
        printf("  OK\n");
    }

    printf("Test flexValidateBuffer accepts a fresh empty flex:\n");
    {
        cleaner(flexFree) flex *fresh = flexNew();
        const size_t bytes = flexBytes(fresh);
        assert(flexValidateBuffer((const uint8_t *)fresh, bytes) == true);
        printf("  OK\n");
    }

    printf("Test flexValidateBuffer accepts a populated small flex:\n");
    {
        cleaner(flexFree) flex *small = flexNew();
        for (int i = 0; i < 32; i++) {
            const databox b = databoxNewSigned(i);
            flexPushSigned(&small, b.data.i, FLEX_ENDPOINT_TAIL);
        }
        const size_t bytes = flexBytes(small);
        assert(flexValidateBuffer((const uint8_t *)small, bytes) == true);
        printf("  OK\n");
    }

    printf("Test flexValidateBuffer rejects mismatched total_bytes:\n");
    {
        /* Buffer claims one length in its header but caller passes a
         * different buflen.  Both directions (too short and too long)
         * must fail. */
        cleaner(flexFree) flex *built = flexNew();
        for (int i = 0; i < 16; i++) {
            const databox b = databoxNewSigned(i);
            flexPushSigned(&built, b.data.i, FLEX_ENDPOINT_TAIL);
        }
        const size_t bytes = flexBytes(built);
        assert(flexValidateBuffer((const uint8_t *)built, bytes - 1) == false);
        assert(flexValidateBuffer((const uint8_t *)built, bytes + 1) == false);
        printf("  OK\n");
    }

    printf("Test flexValidateBuffer rejects truncated header (1B claim,"
           " missing count byte):\n");
    {
        /* A 1-byte total_bytes varint that claims length 1 leaves no
         * room for the count varint that must follow.  Catches the
         * "header-says-OK but body missing" failure shape. */
        const uint8_t truncated[1] = {0x01};
        assert(flexValidateBuffer(truncated, 1) == false);
        printf("  OK\n");
    }

    printf("Test flexValidateBuffer accepts flex spanning header"
           " varint width boundary:\n");
    {
        /* Push enough payload that the total_bytes varint promotes
         * past its 1-byte form, ensuring the multi-byte header path
         * is exercised. */
        cleaner(flexFree) flex *grow = flexNew();
        for (int i = 0; i < 256; i++) {
            const databox b = databoxNewSigned(i);
            flexPushSigned(&grow, b.data.i, FLEX_ENDPOINT_TAIL);
        }
        const size_t bytes = flexBytes(grow);
        assert(bytes > 64); /* past 1-byte varint range */
        assert(flexValidateBuffer((const uint8_t *)grow, bytes) == true);
        printf("  OK\n");
    }

    printf("\n=== All flexValidateBuffer tests passed! ===\n\n");

    /* ================================================================
     * FLEX VALIDATE BUFFER DEEP TESTS
     *
     * flexValidateBufferDeep() is the O(N) strict validator.  Unlike
     * flexValidateBuffer (O(1) envelope-only), it walks every entry's
     * length-encoding bytes and verifies the entry count plus the
     * trailing-byte-budget invariant.  Required by callers that consume
     * flex bytes from un-MAC-gated sources (synthetic fuzzers, plugin
     * blobs, on-disk recovery prior to MAC verification).
     *
     * The function must:
     *   - reject anything flexValidateBuffer already rejects
     *   - accept a well-formed flex of N entries
     *   - reject a buffer whose internal entry length encoding claims
     *     more bytes than remain
     *   - reject a buffer whose header count claim disagrees with the
     *     actual number of entries that fit in the body
     *   - reject a buffer truncated by one byte at the tail
     * ================================================================ */

    printf("Test flexValidateBufferDeep rejects buffers flexValidateBuffer "
           "already rejects:\n");
    {
        /* The deep validator must inherit every rejection the cheap
         * validator already enforces. */
        assert(flexValidateBufferDeep(NULL, 0) == false);
        assert(flexValidateBufferDeep(NULL, 1024) == false);
        const uint8_t empty[1] = {0};
        assert(flexValidateBufferDeep(empty, 1) == false);
        /* total-bytes-claim != buflen */
        cleaner(flexFree) flex *built = flexNew();
        for (int i = 0; i < 16; i++) {
            flexPushSigned(&built, i, FLEX_ENDPOINT_TAIL);
        }
        const size_t bytes = flexBytes(built);
        assert(flexValidateBufferDeep((const uint8_t *)built, bytes - 1) ==
               false);
        assert(flexValidateBufferDeep((const uint8_t *)built, bytes + 1) ==
               false);
        printf("  OK\n");
    }

    printf("Test flexValidateBufferDeep accepts a well-formed flex of N "
           "entries:\n");
    {
        /* Build a real flex with a mix of unsigned ints and byte
         * strings — both exercise the string-varint and fixed-width
         * paths in the safe-walker. */
        cleaner(flexFree) flex *good = flexNew();
        flexPushUnsigned(&good, 1u, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&good, "alpha", 5u, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&good, 0xABCDEFu, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&good, "beta-and-some-padding-bytes", 27u,
                      FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&good, UINT32_MAX, FLEX_ENDPOINT_TAIL);
        const size_t bytes = flexBytes(good);
        assert(flexCount(good) == 5u);
        assert(flexValidateBufferDeep((const uint8_t *)good, bytes) == true);
        printf("  OK\n");
    }

    printf("Test flexValidateBufferDeep rejects entry-length varint claiming "
           "oversize:\n");
    {
        /* Build a valid flex whose first entry is a short byte string,
         * then corrupt that entry's leading length-varint byte to claim
         * a string length far larger than the buffer can hold.  The O(1)
         * envelope check would still pass (header total-bytes and count
         * are intact), but the deep walk must reject. */
        cleaner(flexFree) flex *fb = flexNew();
        flexPushBytes(&fb, "abc", 3u, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&fb, "def", 3u, FLEX_ENDPOINT_TAIL);
        const size_t bytes = flexBytes(fb);
        cleanerName(zfree, Bytes) uint8_t *tampered = zmalloc(bytes);
        memcpy(tampered, fb, bytes);
        /* Header is [total_bytes_varint][count_varint] then entries.
         * Walk past the header to the first entry's encoding byte. */
        const size_t total_bytes_width =
            varintSplitFullNoZeroGetLenQuick_(tampered);
        const size_t count_width =
            varintTaggedGetLenQuick_(tampered + total_bytes_width);
        const size_t entry0_off = total_bytes_width + count_width;
        /* For a small ASCII byte string, byte 0 of the entry holds the
         * split-varint encoding for length 3 (a 1-byte varint).  Tamper
         * it to claim "VAR encoding with an 8-byte external width" —
         * VARINT_SPLIT_FULL_NO_ZERO_VAR (0xC0) | width 8 (0x08) = 0xC8.
         * The encoded varint then "claims" a huge length that overshoots
         * the remaining buffer; the deep walk must reject. */
        tampered[entry0_off] = 0xC8u;
        /* Sanity: cheap validator still passes (envelope intact). */
        assert(flexValidateBuffer(tampered, bytes) == true);
        /* Deep validator catches it. */
        assert(flexValidateBufferDeep(tampered, bytes) == false);
        printf("  OK\n");
    }

    printf("Test flexValidateBufferDeep rejects header count claim that "
           "disagrees with body:\n");
    {
        /* Build a valid 4-entry flex, then increment the tagged count
         * varint so it claims 5 entries; the body still has only 4.
         * The deep walk's per-entry budget check will run out of bytes
         * trying to read entry 5 (since the buffer ends exactly after
         * the four real entries) — must reject. */
        cleaner(flexFree) flex *fc = flexNew();
        for (int i = 1; i <= 4; i++) {
            flexPushUnsigned(&fc, (uint64_t)i, FLEX_ENDPOINT_TAIL);
        }
        const size_t bytes = flexBytes(fc);
        cleanerName(zfree, Bytes) uint8_t *tampered = zmalloc(bytes);
        memcpy(tampered, fc, bytes);
        const size_t total_bytes_width =
            varintSplitFullNoZeroGetLenQuick_(tampered);
        /* For counts in the tagged-varint 1-byte range (≤240), the
         * count byte itself stores the count directly; +1 raises the
         * stored count to 5. */
        assert(tampered[total_bytes_width] == 4u);
        tampered[total_bytes_width] = 5u;
        /* Outer envelope still parses fine — header total-bytes and the
         * count varint width are unchanged; only the count value lies. */
        assert(flexValidateBuffer(tampered, bytes) == true);
        /* Deep walk runs out of bytes when looking for entry 5. */
        assert(flexValidateBufferDeep(tampered, bytes) == false);
        printf("  OK\n");
    }

    printf("Test flexValidateBufferDeep rejects buffer truncated by one "
           "byte:\n");
    {
        /* A buffer the right size with a header that doesn't claim
         * truncation, but missing its last byte, must be rejected.  We
         * build a valid flex, then deliberately tell the deep validator
         * it has one fewer byte than the buffer claims internally.  The
         * envelope check rejects this on the buflen mismatch path,
         * which is exactly what we want — the deep validator inherits
         * that rejection. */
        cleaner(flexFree) flex *fd = flexNew();
        flexPushBytes(&fd, "hello-world", 11u, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&fd, "tail-string", 11u, FLEX_ENDPOINT_TAIL);
        const size_t bytes = flexBytes(fd);
        assert(flexValidateBufferDeep((const uint8_t *)fd, bytes - 1) == false);
        /* Also test the opposite shape: a buffer with the right total
         * bytes in the header but garbage extending past the end.  Pass
         * the deep validator a buflen that mismatches the claim. */
        assert(flexValidateBufferDeep((const uint8_t *)fd, bytes + 1) == false);
        printf("  OK\n");
    }

    printf("\n=== All flexValidateBufferDeep tests passed! ===\n\n");

    /* ================================================================
     * FLEX_RAW_ENTRY_LENGTH_SAFE CORNER CASES (via deep validator)
     *
     * flexRawEntryLengthSafe (src/flex.c:4528) is the bounds-safe twin
     * of flexRawEntryLength; the deep validator (above) is its only
     * caller.  The function is static, so we pin its corner cases by
     * building malformed buffers that pass flexValidateBuffer's O(1)
     * envelope check but trip a SPECIFIC bound check inside the safe
     * walker.  Each corner test stages exactly one rejection path —
     * if a future refactor moves the check or loses a guard, the test
     * still catches the regression because the buffer is constructed
     * to trip only that exact branch.
     *
     * Branches covered:
     *   A) remaining == 0 at entry start (cursor lands at end with
     *      claimed_count still expecting an entry).
     *   B) FORWARD_DECLARE_SUBCONTAINER with remaining < 2 (only the
     *      encoding tag byte fits; the mandatory inner varint width
     *      check has no byte to read).
     *   C) STR encoding whose split-varint width claim exceeds the
     *      buffer remainder (varint header byte says "this varint is
     *      9 bytes wide" but only 1 byte left).
     *   D) STR encoding whose meta + payload total overshoots the
     *      remaining bytes by exactly 1 (payload claimed = remaining,
     *      so meta + payload = remaining + meta > remaining).
     *   E) FORWARD_DECLARE_SUBCONTAINER whose inner-varint width
     *      claim exceeds (remaining - 1), where the encoding tag
     *      consumed byte 0 but the inner varint cannot fit in what's
     *      left.
     * ================================================================ */

    printf("Test flexRawEntryLengthSafe corner: remaining==0 at entry "
           "start (count claims one more than fits):\n");
    {
        /* Build the smallest possible "valid envelope" buffer:
         * total_bytes_varint=1B encoding value 2, count_varint=1B
         * encoding value 1, leaving ZERO bytes for the claimed single
         * entry.  Outer envelope passes (total_bytes_claim == buflen
         * == 2; count varint fits) but the deep walker enters the
         * loop with i=0, remaining=0, and flexRawEntryLengthSafe's
         * first guard (`if (remaining == 0) return false;`) must
         * fire.
         *
         * Encoding details:
         *   - split-varint top 2 bits == 00 -> 1-byte form, low 6
         *     bits store (val - 1).  So byte = (val - 1) for
         *     val in [1, 64].  For total_bytes=2 -> byte = 0x01.
         *   - tagged varint for value 1 is a single byte = 0x01. */
        uint8_t buf[2] = {0x01u, 0x01u};
        /* Sanity: cheap envelope check passes. */
        assert(flexValidateBuffer(buf, sizeof(buf)) == true);
        /* Deep check rejects via the remaining==0 corner. */
        assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        printf("  OK\n");
    }

    printf("Test flexRawEntryLengthSafe corner: FORWARD_DECLARE_SUBCONTAINER "
           "with remaining==1 (tag byte but no room for inner varint):\n");
    {
        /* Build a buffer with one container-tag entry byte and nothing
         * after it.  Layout: total_bytes=3 (1B tot-varint + 1B count
         * + 1B tag), count=1, single entry = container tag.
         * remaining at entry start is exactly 1; the container branch
         * in flexRawEntryLengthSafe needs at least 2 bytes (tag +
         * inner varint header) so the `if (remaining < 2) return false`
         * guard fires. */
        uint8_t buf[3];
        buf[0] = 0x02u; /* split-varint: total_bytes = 3 (byte = 3-1) */
        buf[1] = 0x01u; /* tagged-varint: count = 1 */
        buf[2] = (uint8_t)FLEX_CONTAINER_MAP; /* container tag */
        /* Sanity: cheap envelope passes. */
        assert(flexValidateBuffer(buf, sizeof(buf)) == true);
        /* Deep walker rejects via the container remaining<2 corner. */
        assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        printf("  OK\n");
    }

    printf("Test flexRawEntryLengthSafe corner: STR varint width "
           "exceeds remaining:\n");
    {
        /* Build a buffer whose single entry's encoding byte claims a
         * 9-byte split-varint (top bits 11 + 8-byte external width) =
         * 0xC8.  That requires 9 bytes for the varint alone; we give
         * it only 1 byte (remaining=1 at entry start).  The
         * `if (varint_width > remaining) return false` guard in the
         * STR branch fires. */
        uint8_t buf[3];
        buf[0] = 0x02u; /* split-varint: total_bytes = 3 */
        buf[1] = 0x01u; /* tagged-varint: count = 1 */
        buf[2] = 0xC8u; /* 11001000 — split-varint claiming 9-byte width */
        assert(flexValidateBuffer(buf, sizeof(buf)) == true);
        assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        printf("  OK\n");
    }

    printf("Test flexRawEntryLengthSafe corner: STR meta+payload "
           "overshoots remaining:\n");
    {
        /* Build a buffer whose single STR entry has a 1-byte varint
         * header claiming a payload length that, combined with the
         * mirrored meta bytes (varint_width * 2 = 2 bytes), exceeds
         * the remaining body bytes — exercising the `total > remaining`
         * guard at the end of flexRawEntryLengthSafe.
         *
         * Layout: 1B tot-varint + 1B count + 3B body = 5B total.
         * Entry body starts with a 1-byte STR varint claiming
         * payload_len = 3 (byte = 0x02 since the low-6-bit field
         * stores val-1, decoded as 3).  Entry total = 2 + 3 = 5
         * bytes, but only 3 bytes remain after the envelope header —
         * overshoot by 2. */
        uint8_t buf[5];
        buf[0] = 0x04u; /* split-varint: total_bytes = 5 (byte = 5-1) */
        buf[1] = 0x01u; /* tagged-varint: count = 1 */
        buf[2] = 0x02u; /* STR length-varint claiming payload_len = 3 */
        buf[3] = 0xAAu;
        buf[4] = 0xBBu;
        assert(flexValidateBuffer(buf, sizeof(buf)) == true);
        assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        printf("  OK\n");
    }

    printf("Test flexRawEntryLengthSafe corner: container inner-varint "
           "width exceeds (remaining - 1):\n");
    {
        /* Build a buffer with a container tag followed by an inner
         * varint header byte that claims a 9-byte varint width — but
         * only 1 byte (the header byte itself) is available after the
         * outer tag.  Tag consumes byte 0 of the entry, leaving
         * `remaining - 1` bytes for the inner varint.  The container
         * branch's `if (inner_varint_width > (remaining - 1)) return
         * false` guard fires.
         *
         * Layout: total=4, count=1, entry = [TAG][INNER_HEADER]. */
        uint8_t buf[4];
        buf[0] = 0x03u; /* split-varint: total_bytes = 4 (byte = 4-1) */
        buf[1] = 0x01u; /* tagged-varint: count = 1 */
        buf[2] = (uint8_t)FLEX_CONTAINER_MAP; /* container tag */
        buf[3] = 0xC8u; /* inner split-varint header claiming 9 bytes */
        assert(flexValidateBuffer(buf, sizeof(buf)) == true);
        assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        printf("  OK\n");
    }

    printf("\n=== All flexRawEntryLengthSafe corner tests passed! ===\n\n");

    /* ================================================================
     * VARINT EXTERNAL GET OUT-OF-RANGE WIDTH REJECTION
     *
     * varintExternalGet dispatches into either the little-endian or
     * big-endian uint64_t loader; both have an 8-byte result buffer on
     * the stack.  The underlying byte-copy primitive handles widths
     * 1..16 (the 9..16 widths are reserved for the __uint128_t loaders
     * that own a 16-byte buffer).  If a caller passes width > 8 to the
     * uint64_t loader, the copy would write past the result buffer —
     * an 8-byte stack out-of-bounds write.  The loaders reject widths
     * outside [1, 8] up front: they assert, and return 0 when assertions
     * are compiled out.
     *
     * Whether the vendored varint library keeps its assertions depends
     * on the build type, so this test probes out-of-range widths
     * through varintExternalTryGet, which refuses them in every build.
     * The varint library's own tests cover varintExternalGet's
     * assertion-free fallback.
     * ================================================================ */
    printf("Test varintExternalGet rejects out-of-range widths:\n");
    {
        /* Provide a buffer big enough that, absent the guard, a width=16
         * load would silently read 16 bytes and write all 16 into the
         * 8-byte result; ASAN/UBSAN would catch the stack write. */
        uint8_t buf[16];
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)(i + 1);
        }

        /* Out-of-range widths: 0 (VARINT_WIDTH_INVALID), 9..16 (beyond
         * the uint64_t loaders), and larger values.  Each is refused
         * without writing the output. */
        static const uint32_t badWidths[] = {0,  9,  10, 11, 12, 13,
                                             14, 15, 16, 17, 255};
        for (size_t i = 0; i < sizeof(badWidths) / sizeof(*badWidths); i++) {
            uint64_t out = UINT64_MAX;
            assert(!varintExternalTryGet(buf, (varintWidth)badWidths[i], &out));
            assert(out == UINT64_MAX);
            (void)out;
        }

        /* Sanity: width 1 (in-range) still works.  buf[0] is 0x01. */
        assert(varintExternalGet(buf, VARINT_WIDTH_8B) == 0x01u);
        uint64_t one = 0;
        assert(varintExternalTryGet(buf, VARINT_WIDTH_8B, &one) && one == 1);
        (void)one;
        printf("  OK\n");
    }

    /* ================================================================
     * FLEX VALIDATOR REJECTS ENCODINGS THE SIZE HELPER CANNOT HANDLE
     *
     * FLEX_IS_FORWARD_DECLARE_SUBCONTAINER spans [FLEX_CONTAINER_MAP,
     * FLEX_SAME), which includes the four implemented container tags
     * plus reserved-but-unimplemented values (compressed-container
     * variants 241..244 and the unused gap 245..250).  Only the four
     * implemented tags are sizable by
     * flexDataSizeForFixedWidthEncodingWithInnerEntry; the other
     * values land on its assert(NULL) default.  The validator must
     * reject them so a fuzzed tag byte cannot tunnel through to a
     * fail-loud abort.
     *
     * Similarly the fixed-width range carries two declared-but-unused
     * encodings (FLEX_REAL_32D, FLEX_REAL_64D); the validator must
     * also reject those before they reach the helper.
     * ================================================================ */
    printf("Test flexValidateBufferDeep rejects unimplemented "
           "container encodings:\n");
    {
        /* The four compressed-container reservations (CMAP, CLIST,
         * CSET, CTUPLE) plus the unused gap before FLEX_SAME (245..250)
         * are all in the FORWARD_DECLARE range but unsizable by the
         * helper.  Build a buffer with a single entry whose tag byte
         * is each candidate and confirm the deep validator rejects. */
        const uint8_t bad_tags[] = {
            (uint8_t)FLEX_CONTAINER_CMAP,
            (uint8_t)FLEX_CONTAINER_CLIST,
            (uint8_t)FLEX_CONTAINER_CSET,
            (uint8_t)FLEX_CONTAINER_CTUPLE,
            245u,
            246u,
            247u,
            248u,
            249u,
            250u,
        };
        for (size_t i = 0; i < sizeof(bad_tags) / sizeof(bad_tags[0]); i++) {
            uint8_t buf[4];
            buf[0] = 0x03u;       /* split-varint: total_bytes = 4 */
            buf[1] = 0x01u;       /* tagged-varint: count = 1 */
            buf[2] = bad_tags[i]; /* unsizable container-range tag */
            buf[3] = 0x01u;       /* inner varint header (1-byte form) */
            /* Outer envelope passes (header is well-formed). */
            assert(flexValidateBuffer(buf, sizeof(buf)) == true);
            /* Deep walker rejects via the new helper-allowlist guard. */
            assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        }
        printf("  OK\n");
    }

    printf("Test flexValidateBufferDeep rejects unimplemented "
           "fixed-width encodings (REAL_32D / REAL_64D):\n");
    {
        /* FLEX_REAL_32D / FLEX_REAL_64D are declared but unimplemented;
         * the helper's switch has no case for them and falls through
         * to assert(NULL).  Without the validator guard these would
         * tunnel through. */
        const uint8_t bad_tags[] = {(uint8_t)FLEX_REAL_32D,
                                    (uint8_t)FLEX_REAL_64D};
        for (size_t i = 0; i < sizeof(bad_tags) / sizeof(bad_tags[0]); i++) {
            uint8_t buf[3];
            buf[0] = 0x02u; /* split-varint: total_bytes = 3 */
            buf[1] = 0x01u; /* tagged-varint: count = 1 */
            buf[2] = bad_tags[i];
            assert(flexValidateBuffer(buf, sizeof(buf)) == true);
            assert(flexValidateBufferDeep(buf, sizeof(buf)) == false);
        }
        printf("  OK\n");
    }

    printf("\n=== All width-range and encoding-allowlist tests passed! "
           "===\n\n");

    return flexValidateTotalitySweep();
}
#endif
/* Original ziplist implementation:
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 *
 * Conversion to flex involving refactorings, varints, type get/put APIs,
 * improved tests, restructured element layout, fixing dangerous API usage,
 * adding fast binary search, adding implicit map capability,
 * adding implicit bag capability, adding implicit linear compression, and
 * more:
 * Copyright (c) 2016, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 * notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be
 * used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
