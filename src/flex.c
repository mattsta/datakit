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

#include "str.h"

#include "multimapAtom.h"

#include "conformLittleEndian.h"
#include "util.h"
#include <assert.h>

#include "float16.h"

/* This configuration define allows building flex without 'mdsc', but disabling
 * MDSC support does NOT remove the MDSC types. It only removes the ability for
 * flex to consume MDSC inputs as external references. */
#ifndef FLEX_ENABLE_PTR_MDSC
#define FLEX_ENABLE_PTR_MDSC 1
#endif

#if FLEX_ENABLE_PTR_MDSC
#include "mdsc.h"
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
    FLEX_NEG_8B = FLEX_FIXED_START, /* (FLEX_FIXED_START + 0) == 202 */
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
    /* To avoid duplicating key bytes when using indexes, we allow values to be
     * mdsc entries.
     *
     * We define two storage types.
     * If we can remove the top 16 bits of a pointer, we save 2 bytes and store
     * as FLEX_EXTERNAL_MDSC_48B. If we can't remove the top 2 bytes, we store
     * FLEX_EXTERNAL_MDSC_64B.
     *
     * The benefit of this type is:
     *  - maps are still sorted by the value of what's pointed to, not the value
     *    of the pointer itself.
     *      - BUT NOTE: values must NOT BE CHANGED externally.
     *      - if changing value is required, it must be deleted from map BEFORE
     *        the change
     *      - then re-inserted AFTER the change
     *      - if values are changed by pointer access after insert, subsequent
     *        find behavior won't be able to locate the key (assuming it is a
     *        key (if your entry is a non-key entry, you can change it whenever
     *        and however you like externally). */
    FLEX_EXTERNAL_MDSC_48B,
    FLEX_EXTERNAL_MDSC_64B,

    /************ BEGIN LOGICAL REFERENCE TYPES ************/
    /* 8 reference types.
     *   value following external reference is a logical pointer to some other
     *      data maintained by the user.
     *   We also use these to represent stored pointers, but it's
     *   up to the user to know when a restored databox reference is
     *   a pointer versus an actual user-level reference. */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_8,  /* up to 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_16, /* up to 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_24, /* up to 16 million + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_32, /* up to 4B + 16M + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_40, /* to 1T + 4B + 16M + 64k + 255 */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_48, /* to 281T + 1T + 4B + 16M + ... */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_56, /* to 72PB + 281T + ... */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_64, /* to 18EB + ... */

    /************ BEGIN BOX MARKER TYPES ************/
    /* Note: the order of the next 8 types matter.
     *       The order here must match the order in the databox type enum. */
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

#define EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding)                         \
    ((((encoding) - FLEX_NEG_8B) / FLEX_INTEGER_ENCODING_STEP) + 1)

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
                *_f = zrealloc(*_f, newTotalBytes);
                f = *_f;
            }
        }
    } else {
        /* else, this is a regular resize with no varint grow/shrink */
        if (!drain) {
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
/* Return bytes needed to store fixed type encoded by 'encoding' */
DK_INLINE_ALWAYS size_t flexDataSizeForFixedWidthEncodingWithInnerEntry(
    const flexEncoding encoding, const flexEntry *innerFe,
    varintWidth *encodingSize, const bool isForward) {
    assert(encoding >= FLEX_NEG_8B);

    if (encodingSize) {
        *encodingSize = 1; /* ONE side of the encoding */
    }

    switch (encoding) {
    case FLEX_NEG_8B:
    case FLEX_NEG_16B:
    case FLEX_NEG_24B:
    case FLEX_NEG_32B:
    case FLEX_NEG_40B:
    case FLEX_NEG_48B:
    case FLEX_NEG_56B:
    case FLEX_NEG_64B:
    case FLEX_UINT_8B:
    case FLEX_UINT_16B:
    case FLEX_UINT_24B:
    case FLEX_UINT_32B:
    case FLEX_UINT_40B:
    case FLEX_UINT_48B:
    case FLEX_UINT_56B:
    case FLEX_UINT_64B:
        return EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding);
    case FLEX_UINT_96B:
    case FLEX_NEG_96B:
        return 96 / 8;
    case FLEX_UINT_128B:
    case FLEX_NEG_128B:
        return 128 / 8;
    case FLEX_REAL_B16B:
    case FLEX_REAL_16B:
        return 16 / 8;
    case FLEX_REAL_32B:
        return 32 / 8;
    case FLEX_REAL_64B:
        return 64 / 8;
#if FLEX_ENABLE_PTR_MDSC
    case FLEX_EXTERNAL_MDSC_48B:
        return 48 / 8;
    case FLEX_EXTERNAL_MDSC_64B:
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
        return EXTERNAL_VARINT_WIDTH_FROM_REFERENCE_(encoding);
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
    case FLEX_BYTES_EMPTY:
    case FLEX_TRUE:
    case FLEX_FALSE:
    case FLEX_NULL:
        /* immediate encoding, no value */
        return 0;
    default:
        assert(NULL);
        __builtin_unreachable();
    }
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
        return value - REFERENCE_MAX_48;
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
 * signed zero and can use '0' to store '-1', etc. */
#define SIGNED_PREPARE(v) ((v) + 1)

/* restore the sign bit then go one lower to reverse SIGNED_PREPARE */
#define SIGNED_RESTORE(v) (-(v) - 1)

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

DK_STATIC DK_FN_CONST inline int64_t flexPrepareSignedBig(__int128_t value) {
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

        /* write reverse length */
        varintWidth refWidth;
        varintSplitFullNoZeroReversedPutForward_(fe, refWidth, len);
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
/* Read value pointed to by 'entry', store result in databox 'r' */
DK_STATIC void flexLoadFixedLength_(const flexEntryData *entry, databox *r) {
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
        assert(r->big);
        assert(r->data.i128);
        *r->data.u128 = varintBigExternalGet(d, 96 / 8);
        r->type = DATABOX_UNSIGNED_128;
        break;
    case FLEX_UINT_128B:
        assert(r->big);
        assert(r->data.u128);
        *r->data.u128 = varintBigExternalGet(d, 128 / 8);
        r->type = DATABOX_UNSIGNED_128;
        break;
    case FLEX_NEG_8B:
    case FLEX_NEG_16B:
    case FLEX_NEG_24B:
    case FLEX_NEG_32B:
    case FLEX_NEG_40B:
    case FLEX_NEG_48B:
    case FLEX_NEG_56B:
    case FLEX_NEG_64B:
        varintExternalGetQuick_(
            d, EXTERNAL_VARINT_WIDTH_FROM_ENCODING_(encoding), r->data.i64);
        r->type = DATABOX_SIGNED_64;
        r->data.i64 = SIGNED_RESTORE(r->data.i64); /* restore sign and offset */
        break;
    case FLEX_NEG_96B:
        assert(r->big);
        *r->data.i128 = varintBigExternalGet(d, 96 / 8);
        *r->data.i128 = SIGNED_RESTORE(*r->data.i128);
        r->type = DATABOX_SIGNED_128;
        break;
    case FLEX_NEG_128B:
        assert(r->big);
        *r->data.i128 = varintBigExternalGet(d, 128 / 8);
        *r->data.i128 = SIGNED_RESTORE(*r->data.i128);
        r->type = DATABOX_SIGNED_128;
        break;
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

DK_STATIC int64_t flexLoadSigned(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box);
    return box.data.i64;
}

DK_STATIC __int128_t flexLoadSignedBig(const flexEntryData *entry) {
    databoxBig box = {.big = true};
    flexLoadFixedLength_(entry, (databox *)&box);
    return *box.data.i128;
}

DK_STATIC uint64_t flexLoadUnsigned(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box);
    return box.data.u64;
}

DK_STATIC __uint128_t flexLoadUnsignedBig(const flexEntryData *entry) {
    databoxBig box = {.big = true};
    flexLoadFixedLength_(entry, (databox *)&box);
    return *box.data.u128;
}

DK_STATIC float flexLoadFloat(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box);
    return box.data.f32;
}

DK_STATIC double flexLoadDouble(const flexEntryData *entry) {
    databox box;
    flexLoadFixedLength_(entry, &box);
    return box.data.d64;
}

/* Create a new empty flex. */
flex *flexNew(void) {
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
                                     const uint32_t len,
                                     const uint32_t addCount) {
    const size_t fLen = flexTotalBytes_(*ff);
    *ff = zrealloc(*ff, fLen + len);

    memcpy(*ff + fLen, data, len);
    flexResize(ff, fLen + len, addCount);
}

void flexBulkAppendFlex(flex **const ff, const flex *const zzb) {
    const uint32_t zzbHeader = FLEX_HEADER_SIZE(zzb);
    flexBulkAppend(ff, zzb + zzbHeader, flexTotalBytes_(zzb) - zzbHeader,
                   flexCount(zzb));
}

flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count) {
    size_t totalSize = 0;
    size_t totalCount = 0;
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        totalSize += flexTotalBytes_(fs[i]);
        totalCount += flexCount_(fs[i]);
    }

    flex *f = flexNew();
    flexResize(&f, totalSize, totalCount);

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
        feIsEntryHoldingData = ((fe + FLEX_LENGTH_OF_ENCODING(fe)) == data);   \
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
        abstractWriteFullEntry(&fe, contents[i]);                              \
    }                                                                          \
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
        const uint_fast32_t entrySize = flexRawEntryLength(walkerP);
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

/* Auto-conversion to signed integer or float or double. */
void flexInsertBytes(flex **const ff, flexEntry *const fe, const void *data,
                     size_t len) {
/* 'fe' MUST be inside the bounds of 'f' but not be f itself. */
#if FLEX_DEBUG_EXTENSIVE
    assert(fe != *ff);
    assert(fe && *ff);
    assert(fe > *ff);
    assert(fe <= (*ff + flexTotalBytes_(*ff)));
#endif

    databox box;
    if (StrScanScanReliable(data, len, &box)) {
        flexInsertByType(ff, fe, &box);
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
        const intmax_t i = flexPrepareSigned(box->data.i);
        flexInsert_(ff, fe, flexEncodingSigned(i), &i, sizeof(i));
        break;
    }
    case DATABOX_UNSIGNED_64:
        flexInsert_(ff, fe, flexEncodingUnsigned(box->data.u), &box->data.u,
                    sizeof(box->data.u));
        break;
    case DATABOX_SIGNED_128: {
        assert(box->big);
        const __int128_t add = flexPrepareSignedBig(*box->data.i128);
        flexInsert_(ff, fe, flexEncodingSignedBig(add), &add, sizeof(add));
        break;
    }
    case DATABOX_UNSIGNED_128:
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
    case DATABOX_CONTAINER_CFLEX_MAP:
    case DATABOX_CONTAINER_CFLEX_LIST:
    case DATABOX_CONTAINER_CFLEX_SET:
    case DATABOX_CONTAINER_CFLEX_TUPLE:
        /* TODO: we aren't storing nested compressed maps.
         *       if map is compressed, must uncompress for storage. */
        /* Derive flex encoding from box encoding using enum offset math */
        c->encoding = FLEX_CONTAINER_MAP + DATABOX_CONTAINER_OFFSET(box->type);
        c->data = box->data.bytes.start;         /* flex or cflex */
        c->len = box->len ?: flexBytes(c->data); /* if given, use; else calc */
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
    if (!FLEX_IS_INTEGER(entry.encoding)) {
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

        /* Save new integer with the _original_ encoding, even if original
         * encoding is larger than the optimal encoding for this value. */

        /* flexSaveInteger will recast 'value' to the proper type for
         * writing.
         */
        varintExternalPutFixedWidthQuick_(FLEX_ENTRY_DATA(&entry),
                                          useBox.data.u64, oldEncodingSize);
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
    databox box;
    box.type = DATABOX_SIGNED_64;
    box.data.i64 = value;
    return flexReplaceInteger__(ff, fe, &box);
}

bool flexReplaceUnsigned(flex **const ff, flexEntry *const fe,
                         const uint64_t value) {
    databox box;
    box.type = DATABOX_UNSIGNED_64;
    box.data.u64 = value;
    return flexReplaceInteger__(ff, fe, &box);
}

/* ====================================================================
 * flex in-place incrby
 * ==================================================================== */
bool flexIncrbySigned(flex **const ff, flexEntry *const fe,
                      const int64_t incrby, int64_t *newval) {
    /* Increment by zero — nothing to do! */
    if (incrby == 0) {
        return false;
    }

    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);

    /* Encoding of '0' here probably means you requested to incr
     * on a string with length 1, which is bad... */
    assert(entry.encoding);

    /* Get current value */
    const int64_t value = flexLoadSigned(&entry);

    /* Check for overflow */
    /* TODO: move to built-in overflow intrinsic */
    if ((value < 0 && incrby < (INT64_MIN - value)) ||
        (value > 0 && incrby > (INT64_MAX - value))) {
        return false;
    }

    int64_t incremented = value + incrby;
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
    /* Increment by zero — nothing to do! */
    if (incrby == 0) {
        return false;
    }

    flexEntryData entry;
    flexEntryDataPopulate(fe, &entry);
    if (!FLEX_IS_INTEGER(entry.encoding)) {
        return false;
    }

    /* Get current value */
    uint64_t value = flexLoadUnsigned(&entry);

    uint64_t incremented = value + incrby;

    /* check overflow */

    if (newval) {
        *newval = incremented;
    }

    return flexReplaceUnsigned(ff, fe, incremented);
}

flex *flexDuplicate(const flex *f) {
    const size_t len = flexTotalBytes_(f);
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
        __builtin_unreachable();
    }
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
/* TODO: We could create 'IndexWithMiddle' giving us the ability to traverse
 * closer to our target based on:
 *   - forward from head (first 25% of list),
 *   - reverse from middle (second 25% of list),
 *   - forward from middle (third 25% of list),
 *   - reverse from tail (final 25% of list). */
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

bool flexEntryIsValid(const flex *f, flexEntry *fe) {
    if (fe >= f + flexTotalBytes_(f) || fe < f) {
        return false;
    }

    return true;
}

flexEntry DK_FN_PURE *flexHead(const flex *f) {
    return FLEX_ENTRY_HEAD(f);
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
void flexGetByType(const flexEntry *const fe, databox *box) {
    /* get at fe, populate type in box, populate box, return found */
    flexEntryData entry = {0};
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        box->data.bytes.start = FLEX_ENTRY_DATA(&entry);
        box->len = entry.len;
        box->type = DATABOX_BYTES;
    } else {
        flexLoadFixedLength_(&entry, box);
    }
}

void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const multimapAtom *referenceContainer) {
    /* get at fe, populate type in box, populate box, return found */
    flexEntryData entry = {0};
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        box->data.bytes.start = FLEX_ENTRY_DATA(&entry);
        box->len = entry.len;
        box->type = DATABOX_BYTES;
    } else {
        flexLoadFixedLength_(&entry, box);
        if (FLEX_IS_REF_EXTERNAL(entry.encoding)) {
            /* TODO: make copying version here if atom map is compressed? */
            multimapAtomLookupConvert(referenceContainer, box);
        }
    }
}

void flexGetByTypeCopy(const flexEntry *const fe, databox *box) {
    /* get at fe, populate type in box, populate box, return found */
    flexEntryData entry = {0};
    flexEntryDataPopulate(fe, &entry);
    if (FLEX_IS_STR(entry.encoding)) {
        box->data.bytes.start = zcalloc(1, entry.len);
        memcpy(box->data.bytes.start, FLEX_ENTRY_DATA(&entry), entry.len);
        box->len = entry.len;
        box->type = DATABOX_BYTES;
        box->allocated = true;
    } else {
        flexLoadFixedLength_(&entry, box);
        if (FLEX_IS_FORWARD_DECLARE_SUBCONTAINER(entry.encoding)) {
            /* copy the flex or cflex at 'box->data' to
             * newly allocated memory then attach it to the box. */
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

bool flexGetSigned(flexEntry *fe, int64_t *value) {
    databox box;
    flexGetByType(fe, &box);

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
    flexGetByType(fe, &box);

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
/* Delete entry at 'fe' while maintaining the validity of 'fe' after the
 * deletion (and potential shrink-induced realloc) */
void flexDelete(flex **const ff, flexEntry **fe) {
    assert(ff && *ff && fe && *fe);

    const size_t offset = *fe - *ff;
    flexDelete__(ff, *fe, 1, false);
    *fe = *ff + offset;
}

/* Same as flexDelete() but don't update fe */
void flexDeleteNoUpdateEntry(flex **const ff, flexEntry *fe) {
    assert(fe);

    flexDelete__(ff, fe, 1, false);
}

void flexDeleteDrain(flex **const ff, flexEntry **fe) {
    assert(ff && *ff && fe && *fe);

    const size_t offset = *fe - *ff;
    flexDelete__(ff, *fe, 1, true);

    /* Store pointer to current element in fe, because flexDelete will
     * do a realloc which might result in a different "f"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to FLEX_END, so check this. */
    *fe = *ff + offset;
}

/* Delete 'num' entries from flex starting at 'fe'.
 * Also update *fe in place, to be able to iterate over the
 * flex, while deleting entries. */
void flexDeleteCount(flex **const ff, flexEntry **fe, const uint32_t count) {
    assert(ff && *ff && fe && *fe);

    const size_t offset = *fe - *ff;
    flexDelete__(ff, *fe, count, false);

    /* Store pointer to current element in fe, because flexDelete will
     * do a realloc which might result in a different "f"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to FLEX_END, so check this. */
    *fe = *ff + offset;
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
    const size_t offset = *fe - *ff;
    flexDelete__(ff, *fe, count, true);
    *fe = *ff + offset;
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

    if (FLEX_IS_INTEGER(entry.encoding)) {
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

DK_STATIC bool flexEntryCompareSigned__(const flexEntryData *entry,
                                        const uint8_t *src, const size_t len) {
    DK_NOTUSED(len);

    /* Compare current entry with specified entry */
    if (FLEX_IS_INTEGER(entry->encoding)) {
        const int64_t val = flexLoadSigned(entry);
        if (val == *(int64_t *)src) {
            return true;
        }
    }
    return false;
}

DK_STATIC bool flexEntryCompareUnsigned__(const flexEntryData *entry,
                                          const uint8_t *src,
                                          const size_t len) {
    DK_NOTUSED(len);

    /* Compare current entry with specified entry */
    if (FLEX_IS_INTEGER(entry->encoding)) {
        const uint64_t val = flexLoadUnsigned(entry);
        if (val == *(uint64_t *)src) {
            return true;
        }
    }
    return false;
}

DK_STATIC bool flexEntryCompareString__(const flexEntryData *entry,
                                        const uint8_t *src, const size_t len) {
    if (FLEX_IS_STR(entry->encoding) && (entry->len == len)) {
        return (memcmp(FLEX_ENTRY_DATA(entry), src,
                       entry->len < len ? entry->len : len) == 0);
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
 * flex homogeneous container math
 * ==================================================================== */
#define flexLoopyProcess_(f, doer)                                             \
    do {                                                                       \
        uint_fast32_t _count = flexCount_(f);                                  \
        flexEntry *_fe = FLEX_ENTRY_HEAD(f);                                   \
        while (_count--) {                                                     \
            databox _box;                                                      \
            flexEntryData _entry = {0};                                        \
            flexEntryDataPopulate(_fe, &_entry);                               \
            flexLoadFixedLength_(&_entry, &_box);                              \
            (doer);                                                            \
            _fe = flexNext(f, _fe);                                            \
        }                                                                      \
        return result;                                                         \
    } while (0)

int64_t flexAddSigned(const flex *f) {
    int64_t result = 0;
    flexLoopyProcess_(f, result += _box.data.i);
}

uint64_t flexAddUnsigned(const flex *f) {
    uint64_t result = 0;
    flexLoopyProcess_(f, result += _box.data.u);
}

int64_t flexSubtractSigned(const flex *f) {
    int64_t result = 0;
    flexLoopyProcess_(f, result -= _box.data.i);
}

uint64_t flexSubtractUnsigned(const flex *f) {
    uint64_t result = 0;
    flexLoopyProcess_(f, result -= _box.data.u);
}

int64_t flexMultiplySigned(const flex *f) {
    int64_t result = 1;
    flexLoopyProcess_(f, result *= _box.data.i);
}

uint64_t flexMultiplyUnsigned(const flex *f) {
    uint64_t result = 1;
    flexLoopyProcess_(f, result *= _box.data.u);
}

double flexAddFloat(const flex *f) {
    double result = 1;
    flexLoopyProcess_(f, result += _box.data.f32);
}

double flexSubtractFloat(const flex *f) {
    double result = 0;
    flexLoopyProcess_(f, result -= _box.data.f32);
}

double flexMultiplyFloat(const flex *f) {
    double result = 1;
    flexLoopyProcess_(f, result *= _box.data.f32);
}

double flexAddDouble(const flex *f) {
    double result = 1;
    flexLoopyProcess_(f, result += _box.data.d64);
}

double flexSubtractDouble(const flex *f) {
    double result = 0;
    flexLoopyProcess_(f, result -= _box.data.d64);
}

double flexMultiplyDouble(const flex *f) {
    double result = 1;
    flexLoopyProcess_(f, result *= _box.data.d64);
}

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

    int32_t skipCount = 0;
    flexEntry *head = NULL;
    if (!forward) {
        head = FLEX_ENTRY_HEAD(f);
    }

    const flexEntry *end = GET_END(f);
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
            fe = flexGetPreviousEntry(fe);
            if (fe == head) {
                break;
            }
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
    const bool useHighestInsertPosition) {
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

#if 1
        databox box;
#else
        databoxBig box;
        DATABOX_BIG_INIT(&box);
#endif

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
            if (useReference) {
                flexGetByTypeWithReference(walkingFE, (databox *)&box,
                                           referenceContainer);
            } else {
                flexGetByType(walkingFE, (databox *)&box);
            }

            const int compared =
                databoxCompare((databox *)&box, compareAgainst[i]);
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
        compareElementDepth, false, NULL, false);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedDirectHighest_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, false, NULL, true);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedWithReference_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth,
    const multimapAtom *referenceContainer) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, true, referenceContainer, false);
}

DK_STATIC flexEntry *flexFindPositionByTypeSortedWithReferenceHighest_(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, bool *const found,
    const flexEntry *startMiddle, const uint_fast32_t compareElementDepth,
    const multimapAtom *referenceContainer) {
    return abstractFindPositionByTypeSortedDirect(
        f, elementsPerEntry, compareAgainst, found, startMiddle,
        compareElementDepth, true, referenceContainer, true);
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
    void **keyAsAllocation) {
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

    /* Only replace if we *found* the key.
     * If so, replace everything after the key.  */
    const bool actuallyReplace = compareUsingKeyElementOnly && found;
    const bool isFullWidthReplace = !compareUsingKeyElementOnly;

    /* Most use cases don't have many entries... */
    databox copyBox_[4];
    flexInsertContents content_[4];
    flexInsertContents *contents_[4];
    __int128_t bigBuffer_[4];

    databox *copyBox = copyBox_;
    flexInsertContents *content = content_;
    flexInsertContents **contents = contents_;
    __int128_t *bigBuffer = bigBuffer_;

    if (elementsPerEntry > 4) {
#if 0
        copyBox = zmalloc(elementsPerEntry * sizeof(*copyBox));
        contents = zmalloc(elementsPerEntry * sizeof(*contents));
        content = zmalloc(elementsPerEntry * sizeof(*content));
#else
        /* Optimize this a bit by running one malloc and chopping up the
         * resulting block of memory into what we need... */
        const size_t copyLen = elementsPerEntry * sizeof(*copyBox);
        const size_t contentsLen = elementsPerEntry * sizeof(*contents);
        const size_t contentLen = elementsPerEntry * sizeof(*content);
        const size_t bigBufferLen = elementsPerEntry * sizeof(*bigBuffer);
        void *block =
            zmalloc(copyLen + contentsLen + contentLen + bigBufferLen);
        uint8_t *blockBytes = (uint8_t *)block;
        copyBox = (databox *)blockBytes;
        contents = (flexInsertContents **)(blockBytes + copyLen);
        content = (flexInsertContents *)(blockBytes + copyLen + contentsLen);
        bigBuffer = (__int128_t *)(blockBytes + copyLen + contentLen);
#endif
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

        /* 1 below because the replacement is *after* the key.
         * We don't need to overwrite the key since we already found
         * it, we know it exists, and 'fe' is pointing right at it. */
        flexInsertReplace_____(ff, fe, contents, elementsPerEntry, 1,
                               &headerDiff);
    } else {
        flexInsert_____(ff, fe, contents, elementsPerEntry, &headerDiff);
    }

    /* If running allocated struct buffer, free it. */
    if (copyBox != copyBox_) {
        zfree(copyBox);
    }

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
        false, NULL, false, NULL, false, false, NULL);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly, void **recoveredPointer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        false, NULL, false, NULL, false, true, recoveredPointer);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, flexEntry **const middleEntry,
    const bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        true, referenceContainer, false, NULL, false, false, NULL);
}

bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
    flex **const ff, const uint_fast32_t elementsPerEntry,
    const databox **const box, const databox *const boxInsertKey,
    flexEntry **const middleEntry, const bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer) {
    return abstractInsertReplaceByTypeSortedWithMiddleMulti(
        ff, elementsPerEntry, box, middleEntry, compareUsingKeyElementOnly,
        true, referenceContainer, true, boxInsertKey, false, false, NULL);
}

void flexAppendMultiple(flex **ff, const uint_fast32_t elementsPerEntry,
                        const databox **const box) {
    if (elementsPerEntry == 0) {
        return;
    }

    /* TODO: verify non-auto-array these */
    databox copyBox[elementsPerEntry];
    flexInsertContents *contents[elementsPerEntry];
    flexInsertContents content[elementsPerEntry];
    __int128_t bigBuffer[elementsPerEntry];

    for (uint_fast32_t i = 0; i < elementsPerEntry; i++) {
        copyBox[i] = *box[i];
        insertContentsFromBox(&copyBox[i], &content[i],
                              CONVERSION_OVERRIDE_NONE, i, bigBuffer);
        contents[i] = &content[i];
    }

    flexInsert_____(ff, FLEX_ENTRY_END(*ff), contents, elementsPerEntry, NULL);
}

int flexCompareEntries(const flex *const f,
                       const databox *const *const elements,
                       const uint_fast32_t elementsPerEntry,
                       const int_fast32_t offset) {
    flexEntry *fCompare = flexIndex(f, offset * elementsPerEntry);
    databox box;

    for (uint_fast32_t i = 0; i < elementsPerEntry; i++) {
        flexGetByType(fCompare, &box);
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
    flexFindForward_((uint8_t *)&val, len, flexEntryCompareString__);
}

flexEntry *flexFindStringReverse(const flex *const f, flexEntry *const fe,
                                 const void *val, const size_t len,
                                 const uint32_t skip) {
    flexFindReverse_((uint8_t *)&val, len, flexEntryCompareString__);
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
    //        flexFind__(forward, (uint8_t *)&box->data.f32,
    //        sizeof(box->data.f32), flexEntryCompareFloat__);
    //        break;
    case DATABOX_DOUBLE_64:
    //        flexFind__(forward, (uint8_t *)&box->data.d64,
    //        sizeof(box->data.d64), flexEntryCompareDouble__);
    //        break;
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
        return NULL; /* not implemented yet! Need to refactor finding to
                        support
                        finding by encoding and not just finding by value
                        (since
                        immediate encodings have no values). */
    default:
        return f;
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

#if 0
/* Legacy Find function */
flexEntry *flexFind(flexEntry *fe, const void *vstr, uint32_t vlen,
        uint32_t skip) __attribute__((deprecated));
/* Start at flexEntry 'fe' and find pointer to the entry equal to the specified
 * entry. Skip 'skip' entries between every comparison.
 * Returns NULL when the field could not be found. */
flexEntry *flexFind(flexEntry *fe, const void *vstr, uint32_t vlen,
        uint32_t skip) {
    int32_t skipCount = 0;
    int64_t vll = 0;
    bool processedEncoding = false;
    bool encoded = false;

    if (!fe) {
        return NULL;
    }

    while (!IS_END(fe)) {
        assert(NULL && "fix me");
        uint8_t prevlensize = 0;
        uint8_t lensize = 0;
        size_t len;

        flexEntryData e = {0};
        flexEntryDataPopulate(fe + prevlensize, &e);
        lensize = e.encodingSize;
        len = e.len;

        flexEntry *q = fe + prevlensize + lensize;

        if (skipCount == 0) {
            /* Compare current entry with specified entry */
            if (FLEX_IS_STR(e.encoding)) {
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return fe;
                }
            } else {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                if (!processedEncoding) {
                    if (vlen <= 32 && string2ll(vstr, vlen, &vll)) {
                        encoded = true;
                    }

                    processedEncoding = true;
                }

                /* Compare current entry with specified entry, do it only
                 * if we are encoded because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (encoded) {
                    int64_t ll = 3; // flexLoadSigned(q, encoding);
                    assert(NULL);
                    if (ll == vll) {
                        return fe;
                    }
                }
            }

            /* Reset skip count */
            skipCount = skip;
        } else {
            /* Skip entry */
            skipCount--;
        }

        /* Move to next entry */
        fe = q + len;
    }

    return NULL;
}
#endif

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
            } else if (FLEX_IS_INTEGER(entry.encoding) &&
                       entry.encoding < FLEX_UINT_64B) {
                printf("[#:%" PRId64 "]", flexLoadSigned(&entry));
            } else if (FLEX_IS_INTEGER(entry.encoding) &&
                       entry.encoding == FLEX_UINT_64B) {
                printf("[#:%" PRIu64 "]", flexLoadUnsigned(&entry));
            } else if (FLEX_IS_INTEGER_BIG(entry.encoding)) {
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

static flex *createList() {
    flex *f = flexNew();
    flexPushBytes(&f, "foo", 3, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "quux", 4, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_HEAD);
    flexPushBytes(&f, "1024", 4, FLEX_ENDPOINT_TAIL);
    return f;
}

static flex *createIntList() {
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
        flex *f = flexNew();
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
        zfree(f);
        total += end - start;
    }

    return total;
}

static uint64_t stressReplaceInline(flexEndpoint pos, int32_t num,
                                    int32_t maxsize, int32_t dnum) {
    static const char location[2][5] = {"TAIL", "HEAD"};
    uint64_t total = 0;
    for (int32_t i = 0; i < maxsize; i += dnum) {
        flex *f = flexNew();
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
        zfree(f);
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

#include "jebuf.h"
#include <locale.h>
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

    flex *f = NULL;
    flexEntry *fe = NULL;
    (void)f;
    (void)fe;

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

        flex *split = flexDuplicate(f);
        flex *secondHalf = flexSplit(&split, 2);

        flexFree(secondHalf);
        flexFree(split);

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

            flex *split = flexDuplicate(f);
            flex *secondHalf = flexSplit(&split, 2);

            flexFree(secondHalf);
            flexFree(split);

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
        int *vals = zcalloc(totalVals, sizeof(*vals));
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
                    fs[0] = flexNew();
                    fs[0] = zrealloc(fs[0], grow * sizeof(int) * 2);
                    fs[1] = flexNew();
                    fs[1] = zrealloc(fs[1], grow * sizeof(int) * 2);
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
                    f = flexNew();
                    f = zrealloc(f, grow * 4);

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

                /* LOOKUP */
                size_t looper = totalVals * 1024;
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
                    flex *split = flexDuplicate(f);
                    flex *secondHalf = flexSplit(&split, 1);

                    flexFree(secondHalf);
                    flexFree(split);

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
        zfree(vals);
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
#define mku(u_) {.data.u = (u_), .type = DATABOX_UNSIGNED_64}
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

        flexAppendMultiple(&f, 10, fields);

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
            flex *f2 = flexDuplicate(f);
            flex *f3 = flexDuplicate(f);
            flex *f4 = flexDuplicate(f);

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

            flexFree(f4);
            flexFree(f3);
            flexFree(f2);
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

        flex *f3 = flexNew();
        flex *f4 = flexNew();

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

        flexFree(f4);

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
        flex *f22 = flexDuplicate(f2);
        f22 = flexMerge(&f22, &f2);
        flexRepr(f22);
        flexFree(f22);

        printf("SUCCESS\n\n");
    }

    printf("Test merge and place large entries:\n");
    {
        f = createList();
        flex *f2 = createList();

        f = flexMerge(&f, &f2);
        flex *orig = flexDuplicate(f);

        size_t biglen = 64 * 1024 * 1024; /* 64 MB */
        uint8_t *big = zmalloc(biglen);
        randbytes(big, biglen, biglen);
        databox box = databoxNewBytes(big, biglen);

        char smaller[42] = {7};
        size_t smalllen = sizeof(smaller);
        databox smallbox = databoxNewBytes(smaller, smalllen);

        flexRepr(f);
        printf("Inserting and deleting large elements in random spots:\n");
        for (int32_t i = 0; i < 32; i++) {
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
        flexFree(orig);
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

    printf("Create long list and check indices:\n");
    {
        f = flexNew();
        char buf[32];
        int64_t len;

        int64_t start = 0;
#if defined(TEST_MEMORY_BIG) || !__has_feature(address_sanitizer)
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
        for (int64_t i = start; i < loops; i++) {
            if (i % (9000) == 0) {
                printf("Validating %" PRIi64 " (%" PRIi64 " remaining)\n", i,
                       (loops - i));
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
        void *compressRestoreBuffer = zcalloc(1, compressRestoreSize);
        cflex *c = (cflex *)compressRestoreBuffer;
        flex *restored = zcalloc(1, compressRestoreSize);

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

        zfree(restored);
        zfree(compressRestoreBuffer);
    }

#if defined(TEST_MEMORY_BIG) || !__has_feature(address_sanitizer)
    printf("Create flex in single byte increments up to 8 M entries:\n");
#else
    printf("Create flex in single byte increments up to 128 k entries:\n");
#endif
    {
        f = flexNew();

#if defined(TEST_MEMORY_BIG) || !__has_feature(address_sanitizer)
        const int32_t contentsSize = 1024 * 1024 * 8; /* 8 MB */
#else
        const int32_t contentsSize = 1024 * 128; /* 128 k */
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
            char *buf = zcalloc(1, bufbuflen);
/* If using a big memory test
 *   - or -
 * If not running under ASan */
#if defined(TEST_MEMORY_BIG) || !__has_feature(address_sanitizer)
            const int numloops = 25000;
#else
            /* else, (ASan && !TEST_MEMORY_BIG), use fewer loops. */
            const int numloops = 250;
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

                f = flexNew();
                if (preallocate) {
                    /* preallocate 16 MB of memory */
                    f = zrealloc(f, 1 << 24);
                }

                list *ref = listCreate();
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
                            /* signed (randomly negative) big integers */
                            buflen = snprintf(
                                buf, bufbuflen, "%" PRId64,
                                (int64_t)(rand() % 2 ? -1 : 1) *
                                        (int64_t)(0LL + rand())
                                    /* triggers sanitizer but not a problem */
                                    << 45);
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
                        listAddNodeHead(ref, mdscnewlen(buf, buflen));
                    } else if (where == FLEX_ENDPOINT_TAIL) {
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
                    flexGetByType(fe, &got);

                    if (got.type == DATABOX_BYTES) {
                        str = got.data.bytes.start;
                        len = got.len;
                        buflen = len;
                        memcpy(buf, str, buflen);
                    } else if (got.type == DATABOX_SIGNED_64) {
                        buflen =
                            snprintf(buf, bufbuflen, "%" PRIdMAX, got.data.i);
                    } else if (got.type == DATABOX_UNSIGNED_64) {
                        buflen =
                            snprintf(buf, bufbuflen, "%" PRIuMAX, got.data.u);
                    } else if (got.type == DATABOX_FLOAT_32) {
                        buflen = StrDoubleFormatToBufNice(buf, bufbuflen,
                                                          got.data.f32);
                    } else if (got.type == DATABOX_DOUBLE_64) {
                        buflen = StrDoubleFormatToBufNice(buf, bufbuflen,
                                                          got.data.d64);
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
                listRelease(ref);
            }

            uint64_t tend = timeUtilUs();
            zfree(buf);

            printf("SUCCESS (%f sec total; %f sec deletion; %'zu total "
                   "bytes)\n\n",
                   ((double)tend - tstart) / 1e6, (double)deletion / 1e6,
                   totalAllAllocated);
        }
    }

    printf("Stress with variable flex sizes (insert + delete):\n");
    {
        float headTotal = stress(FLEX_ENDPOINT_HEAD, 100000, 16384, 256);
        float tailTotal = stress(FLEX_ENDPOINT_TAIL, 100000, 16384, 256);

        printf("SUCCESS (%f sec head)\nSUCCESS (%f sec tail)\n\n",
               headTotal / 1e6, tailTotal / 1e6);
    }

    printf("Stress with variable flex sizes (insert replace):\n");
    {
        float headTotal =
            stressReplaceInline(FLEX_ENDPOINT_HEAD, 100000, 16384, 256);
        float tailTotal =
            stressReplaceInline(FLEX_ENDPOINT_TAIL, 100000, 16384, 256);

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
        int64_t *oracle = zcalloc(count, sizeof(int64_t));

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

        zfree(oracle);
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
        int64_t *oracle = zcalloc(maxSize, sizeof(int64_t));
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

        zfree(oracle);
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
        uint8_t *exists = zcalloc((keySpace + 7) / 8, 1);
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

        zfree(exists);
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
        char *str16446 = zmalloc(16447);
        char *str16447 = zmalloc(16448);
        char *str16448 = zmalloc(16449);

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

        zfree(str16446);
        zfree(str16447);
        zfree(str16448);
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
        printf("  OK\n");
    }

    printf("\n=== All varint encoding boundary tests passed! ===\n\n");

    return 0;
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
