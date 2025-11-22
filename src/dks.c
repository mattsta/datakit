/* dks — datakit string - a modernized modern string buffer
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

#include "../deps/varint/src/varintExternalBigEndian.h"

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include "../deps/xxHash/xxhash.h"

#include "dks.h"
#include "fibbuf.h"

#include <inttypes.h>
#include "jebuf.h"
#include "str.h"
#include "strDoubleFormat.h"

/* Type Definitions */

/* NOTE! Our data layout is: [LEN][FREE_TYPE][BYTES...]
 *
 * 'free_type' holds both the free size of 'buf' and the type of this dks.
 *
 * We always use big endian integers for 'free_type', so the type data is always
 * in the last two or three bits of the integer. e.g. [XXXXXXYY] and [XXXXXYYY]
 *
 * Storing type information in the same integer as the free length
 * means the free length is now limited to two (or three) bits less than the
 * storage size of 'free_type'.
 * The dks will automatically grow to the larger type when
 * more space is needed to stay within the bounds of our defined limits.
 *
 * The reason for manual packing of 'type' inside 'free_type' is because
 * we can't use any type-obtaining abstraction due to the width of
 * 'free_type' being variable between 1 byte and 8 bytes.
 * We also can't pick one storage order then use a generic
 * "reverse bytes" function because the number of bytes to reverse changes
 * depending on the dks type, and we only know the type after we read the
 * type information stored in the place we would need to reverse anyway.
 */

#define UINT24_MAX 0xffffff
#define UINT40_MAX 0xffffffffff
#define UINT48_MAX 0xffffffffffff

/* Define:
 *   - MAX:
 *     - maximum length of dks for storage class
 *   - FULL_MAX:
 *     - maximum length of content
 *     - maximum length of a full width integer for the storage class.
 *   - SHARED_MAX:
 *     - max recorded size of unused space; i.e. malloc'd but not in 'len'
 *     - maximum length of integer minus type bits for the storage class. */

#define DKS_TYPE_BITS_2 2
#define DKS_TYPE_BITS_3 3

#define DKS_8_MAX ((uint8_t)UINT8_MAX)
#define DKS_8_FULL_MAX (DKS_8_MAX)
#define DKS_8_SHARED_MAX (DKS_8_MAX >> DKS_TYPE_BITS_2)

#define DKS_16_MAX ((uint16_t)UINT16_MAX)
#define DKS_16_FULL_MAX (DKS_16_MAX)
#define DKS_16_SHARED_MAX (DKS_16_MAX >> DKS_TYPE_BITS_2)

#define DKS_24_MAX ((uint32_t)UINT24_MAX)
#define DKS_24_FULL_MAX (DKS_24_MAX)
#define DKS_24_SHARED_MAX (DKS_24_MAX >> DKS_TYPE_BITS_3)

#define DKS_32_MAX ((uint32_t)UINT32_MAX)
#define DKS_32_FULL_MAX (DKS_32_MAX)
#define DKS_32_SHARED_MAX (DKS_32_MAX >> DKS_TYPE_BITS_3)

#define DKS_40_MAX ((uint64_t)UINT40_MAX)
#define DKS_40_FULL_MAX (DKS_40_MAX)
#define DKS_40_SHARED_MAX (DKS_40_MAX >> DKS_TYPE_BITS_3)

#define DKS_48_MAX ((uint64_t)UINT48_MAX)
#define DKS_48_FULL_MAX (DKS_48_MAX)
#define DKS_48_SHARED_MAX (DKS_48_MAX >> DKS_TYPE_BITS_3)

/* Big Endian type definitions:
 *   - mask for 2 bits
 *   - mask for 3 bits
 *   - mask to check if lowest bit is 0 or 1 */
#define DKS_TYPE_2_MASK 0x03             /* 00000011 */
#define DKS_TYPE_3_MASK 0x07             /* 00000111 */
#define DKS_TYPE_DETERMINATION_MASK 0x01 /* 00000001 */
typedef enum dksType {
    /* Types described using 2 bits (byte ends with 0) */
    DKS_8 = 0x00,  /* 00000000 */
    DKS_16 = 0x02, /* 00000010 */
    /* Types described using 3 bits (byte ends with 1) */
    DKS_24 = 0x01, /* 00000001 */
    DKS_32 = 0x03, /* 00000011 */
    DKS_40 = 0x05, /* 00000101 */
    DKS_48 = 0x07, /* 00000111 */
} dksType;
/* We don't add DKS_56 and DKS_64 because DKS_40 is already
 * able to address up to 1 TB of contiguous memory and
 * DKS_48 can address up to 281 TB of contiguous memory when
 * used as full lengths.  When used as shared lengths, DKS_40
 * can address up to 137 GB and DKS_48 can address up to 35 TB.
 * It seems unlikely we will be creating in-memory strings
 * larger than 35 TB or 281 TB at our present historical juncture. */

#define DKS_TYPE_DIFF_2 (DKS_16 - DKS_8)
#define DKS_TYPE_DIFF_3 (DKS_32 - DKS_24)

/* If we are the same bit type, compare directly.
 * else if 'from' is 2-bit and 'to' is 3-bit, we grew.
 * else, remaining configuration: 'from' is 3-bit and 'to' is 2 bit = shrink */
#define _dksGrewFromTo(from, to)                                               \
    ((((from) & DKS_TYPE_DETERMINATION_MASK) ==                                \
      ((to) & DKS_TYPE_DETERMINATION_MASK))                                    \
         ? (to) > (from)                                                       \
     : (_dksTypeIsTwoBits(from) && !_dksTypeIsTwoBits(to)) ? true              \
                                                           : false)

/* dksInfo = dks information for administrative tasks.
 *   (8 + 8 + 8 + 8 + 4) = 36 byte struct:
 *     - 64-bit start pointer
 *     - 64-bit buf pointer
 *     - 64-bit length
 *     - 64-bit free
 *     - 32-bit type enum */
typedef struct dksInfo {
    uint8_t *start;
    DKS_TYPE *buf;
    size_t len;
    size_t free;
    dksType type;
} dksInfo;

/* Code */

DK_STATIC void DKS_SETPREVIOUSINTEGERANDTYPE(DKS_TYPE *s, const size_t free,
                                             const dksType type);
DK_STATIC size_t DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(const DKS_TYPE *s,
                                                       dksType type);

DK_STATIC size_t DKS_MAXSHAREDFORTYPE(dksType type) {
    switch (type) {
    case DKS_8:
        return DKS_8_SHARED_MAX;
    case DKS_16:
        return DKS_16_SHARED_MAX;
    case DKS_24:
        return DKS_24_SHARED_MAX;
    case DKS_32:
        return DKS_32_SHARED_MAX;
    case DKS_40:
        return DKS_40_SHARED_MAX;
    case DKS_48:
        return DKS_48_SHARED_MAX;
    }

    assert(NULL);
    __builtin_unreachable();
}

/* If type is even, it's a 2 bit type, otherwise, it's a 3 bit type. */
#define _dksTypeIsTwoBits(type) (((type) & DKS_TYPE_DETERMINATION_MASK) == 0)

/* Since our types are encoded as fixed offset integers, we can
 * use {{math}} to determine metadata sizes instead of needing
 * to use a lookup table.
 *
 * Basically:
 *   For 3 bit types (DKS_24, DKS_32, DKS_40, DKS_48),
 *      we subtract the target type from our first 3 bit type (DKS_24),
 *      then we divide by the fixed offsets between each 3 bit types
 *      (DKS_32 - DKS_24 just gives us the offset between all 3 bit types)
 *      then we add 3 because our 3 bit types start at 3 bytes of usage.
 *      This "normalizes" all 3 bit types to how many bytes they use based
 *      on the first 3 bit type.
 *   For 2 bit types (DKS_8, DKS_16),
 *      we subtract the target type from our first 2 bit type (DKS_8),
 *      then we divide by the fixed offsets between our 2 bit types
 *      (DKS_16 - DKS_8 is the fixed offset)
 *      then we add 1 because our 2 bit types start at 1 byte of usage. */
#define _dksHeaderElementSize(type)                                            \
    (_dksTypeIsTwoBits(type) ? ((((type)-DKS_8) / DKS_TYPE_DIFF_2) + 1)        \
                             : ((((type)-DKS_24) / DKS_TYPE_DIFF_3) + 3))

/* Grab the 2 MASK bits from (buf - 1) */
#define DKS_TYPE_2(buf) ((uint8_t)((*((buf)-1) & DKS_TYPE_2_MASK)))

/* Grab the 3 MASK bits from (buf - 1) */
#define DKS_TYPE_3(buf) ((uint8_t)((*((buf)-1) & DKS_TYPE_3_MASK)))

/* if type determination bit is 0, extract 2 bits, else extract 3 bits. */
#define DKS_TYPE_GET(buf)                                                      \
    (_dksTypeIsTwoBits((uint8_t) * ((buf)-1)) ? DKS_TYPE_2(buf)                \
                                              : DKS_TYPE_3(buf))

#define TYPEBITS(type) (_dksTypeIsTwoBits(type) ? 2 : 3)

/* Set 'val' with bottom TYPEBITS set to 'type' */
DK_STATIC void DKS_SETPREVIOUSINTEGERANDTYPE(DKS_TYPE *restrict s,
                                             const size_t val,
                                             const dksType type) {
    const uint8_t elementSize = _dksHeaderElementSize(type);

    /* Set previous integer after shifting up to store type bits. */
    const size_t finalPrevious = (val << TYPEBITS(type)) | type;
    varintExternalBigEndianPutFixedWidthQuick_(s - elementSize, finalPrevious,
                                               elementSize);
}

DK_STATIC size_t DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED(
    const DKS_TYPE *restrict s, const dksType type) {
    const uint8_t elementSize = _dksHeaderElementSize(type);

    size_t result = 0;
    varintExternalBigEndianGetQuick_(s - elementSize, elementSize, result);

    /* Return previous integer after shifting away type bits. */
    return result >> TYPEBITS(type);
}

#if defined(DATAKIT_DKS_COMPACT)
#include "dksCompact.h"
#define DKS_IMPL 'C'
#define DKS_IMPL_STR "COMPACT"
#elif defined(DATAKIT_DKS_FULL)
#include "dksFull.h"
#define DKS_IMPL 'F'
#define DKS_IMPL_STR "FULL"
#else
#error "No DK type defined!"
#endif

/* New empty (zero length) dks string. Even in this case the string
 * always has an implicit null term. */
DKS_TYPE *DKS_EMPTY(void) {
    return DKS_NEWLEN(NULL, 0);
}

DKS_TYPE *DKS_EMPTYLEN(size_t len) {
    return DKS_NEWLEN(NULL, len);
}

/* New dks starting from null terminated C string. */
DKS_TYPE *DKS_NEW(const char *init) {
    const size_t initlen = (init == NULL) ? 0 : strlen(init);
    return DKS_NEWLEN(init, initlen);
}

size_t DKS_LEN(const DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    return info.len;
}

DKS_TYPE *DKS_DUP(const DKS_TYPE *s) {
    return DKS_NEWLEN(s, DKS_LEN(s));
}

void DKS_FREE(DKS_TYPE *s) {
    if (s) {
        zfree(DKS_BASE(s, NULL));
    }
}

__attribute__((optnone)) void DKS_FREEZERO(DKS_TYPE *s) {
    if (s) {
#if __STDC_LIB_EXT1__
        /* Guaranteed memory zero out, but only in C11 mode
         * and only on platforms with the correct headers
         * and libc and whatnot */
        const size_t len = DKS_LEN(s);
        memset_s(s, len, 0, len);
#else
        /* Note: the compiler may notice we're modifying
         * data then just freeing it, which could allow the
         * compiler to not zero out the memory since it
         * isn't going to be accessed again. */
        memset(s, 0, DKS_LEN(s));
#endif
        zfree(DKS_BASE(s, NULL));
    }
}

/* Remove dks header so we can use buffer as malloc'd byte array.
 * Returns the contents of 's' usable as a full malloc'd C string. */
uint8_t *DKS_NATIVE(DKS_TYPE *s, size_t *len) {
    dksInfo info;
    uint8_t *base = (uint8_t *)(DKS_BASE(s, &info));
    s = NULL;

    if (len) {
        *len = info.len;
    }

    memmove(base, info.buf, info.len);

    /* We previously used "return zrealloc(base, info.len);", but
     * the caller may not need their DKS_NATIVE realloc()'d.
     * We now allow the caller to retrieve the length of the
     * allocated string so they can realloc manually if needed. */

    /* Return pointer to start of dks allocation instead of to
     * dks->buf, which is offset 2 to 16 bytes into the allocation. */
    return base;
}

/* Convenience wrappers for _DKS_INFOUPDATELENFREE() */
DK_STATIC void DKS_INFOUPDATELENFREE(dksInfo *info, size_t len, size_t free) {
    DKS__INFOUPDATELENFREE(info, len, free, true);
}

DK_STATIC void DKS_INFOUPDATELENFREENOTERMINATE(dksInfo *info, size_t len,
                                                size_t free) {
    DKS__INFOUPDATELENFREE(info, len, free, false);
}

/* For these two wrappers, caller knows 'len' fits inside current dks type
 * by calling DKS_EXPANDBY before increasing length.
 *
 * Returns new total length of dks. */
DK_STATIC size_t DKS_INCREASELENGTHBY(dksInfo *info, ssize_t len) {
    DKS__INFOUPDATELENFREE(info, info->len + len, info->free - len, true);
    return info->len;
}

DK_STATIC size_t DKS_INCREASELENGTHBYNOTERMINATE(dksInfo *info, ssize_t len) {
    DKS__INFOUPDATELENFREE(info, info->len + len, info->free - len, false);
    return info->len;
}

/* Read filename and populate new DKS */
DKS_TYPE *DKS_NEWFROMFILE(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);

    DKS_TYPE *t = DKS_NEWLEN(NULL, length);
    if (!t) {
        fclose(fp);
        return NULL;
    }

    /* Read contents of 'filename' into 't' */
    if (fread(t, 1, length, fp) != (size_t)length) {
        DKS_FREE(t);
        fclose(fp);
        return NULL;
    }

    fclose(fp);

    return t;
}

uint64_t DKS_XXH64(const DKS_TYPE *s, const uint64_t seed) {
    dksInfo info;
    DKS_BASE(s, &info);

    return XXH64(info.start, info.len, seed);
}

uint64_t DKS_XXH3_64(const DKS_TYPE *s, const uint64_t seed) {
    dksInfo info;
    DKS_BASE(s, &info);

    return XXH3_64bits_withSeed(info.start, info.len, seed);
}

__uint128_t DKS_XXH3_128(const DKS_TYPE *s, const uint64_t seed) {
    dksInfo info;
    DKS_BASE(s, &info);

    XXH128_hash_t result = XXH3_128bits_withSeed(info.start, info.len, seed);
    return ((__uint128_t)result.high64 << 64) | result.low64;
}

void DKS_UPDATELENFORCE(DKS_TYPE *s, size_t newlen) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    DKS_INFOUPDATELENFREENOTERMINATE(&info, newlen,
                                     info.free + info.len - newlen);
}

void DKS_UPDATELEN(DKS_TYPE *s) {
    /* Length = actual strlen;
     * Free = (original free) + (original length) - (newly assigned length) */
    DKS_UPDATELENFORCE(s, strlen(s));
}

/* Make data length 0 and convert all previous data length to free space. */
void DKS_CLEAR(DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    /* Clear means length = 0 then free += length */
    size_t newLen = 0;
    size_t newFree = info.free + info.len;

    dksType ideal = DKS_CONTAINERTYPE(newLen, newFree);
    if (ideal > info.type) {
        /* The value 'newFree' doesn't fit in the existing
         * allocation size!
         * Since this API function doesn't
         * return the buffer, we can't realloc anything.
         * So, we just set free to SHARED_MAX for current type.
         * Note: this "hides" some free space from the user,
         * but if the user needs a larger buffer, they will
         * ExpandBy then the realloc will still use
         * the extra space to expand the dks allocation. */
        newFree = DKS_MAXSHAREDFORTYPE(info.type);
    }

    DKS_INFOUPDATELENFREE(&info, newLen, newFree);
}

size_t DKS_AVAIL(const DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    return info.free;
}

size_t DKS_BUFALLOCSIZE(const DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    return info.len + info.free;
}

/* Grow dks free space and optionally upgrade dks to larger type
 * if the current type can't hold the requested new length. */
DK_STATIC void DKS_UPGRADEFREEALLOCATION(dksInfo *current,
                                         size_t newSizeOfDataAllocation) {
    size_t newFree = newSizeOfDataAllocation - current->len;

    dksType useType = current->type;
    bool upgradeContainer = false;
    size_t newTotalSize = 0;
    uint32_t newHeaderSize = 0;
    dksType target;

    /* Note: we do the following determiniation in a loop because
     *       adding sizes can cause us to need a larger container,
     *       which means we need to re-evaulate the same conditions
     *       all over again. */
    while (true) {
        /* If target type is larger than current type, we must upgrade
         * the container in addition to just increasing space. */
        target = DKS_CONTAINERTYPE(newSizeOfDataAllocation, newFree);
        if (_dksGrewFromTo(current->type, target)) {
            upgradeContainer = true;
            useType = target;
        }

        /* Operations for growing:
         *   - reallocate (grow) to new size.
         *   - set 'dks' pointer to current buf. */
        newHeaderSize = _dksHeaderSize(useType);

        newTotalSize = newHeaderSize + newSizeOfDataAllocation + 1;
        const size_t newAllocationSize = jebufSizeAllocation(newTotalSize);

        if (newAllocationSize > newTotalSize) {
            newTotalSize = newAllocationSize;

            /* Grow available space up to our new allocation size */
            newSizeOfDataAllocation = newTotalSize - newHeaderSize - 1;

            /* Free space is: allocation size with used data not considered. */
            newFree = newSizeOfDataAllocation - current->len;

            /* We changed the base conditions of all the above calculations,
             * so we need to run them all again. */
            continue;
        }

        /* else, we found a stable allocation size bucket.
         * no more growth necessary. */
        break;
    }

    current->start = zrealloc(current->start, newTotalSize);
    current->buf = (DKS_TYPE *)(current->start + newHeaderSize);

    /* Operations for upgrading:
     *   - move old buffer to new buffer offset (including trailing '\0').
     *   - set current type so UpdateLenFree can reset type properly. */
    if (upgradeContainer) {
        /* Upgrade math example:
         *   - smaller (uint8, uint8, char[]) to small (uint16, uint16, char[])
         *   - smaller char[] is offset by 2 bytes.
         *   - small char[] is offset by 4 bytes.
         *   - We need the offset of the *current* char[] so we can move it to
         *     the new offset for the target type.
         *   - So, calculate (START + [OLD char[] OFFSET]) then move those bytes
         *     to current->buf.
         *   - Length is the dks length + 1 for the trailing '\0' */
        int oldBufOffset = _dksHeaderSize(current->type);
        memmove(current->buf, current->start + oldBufOffset, current->len + 1);
        current->type = target;
    }

    /* Now update 'len' and 'free' (with implicit 'type' update) */
    DKS_INFOUPDATELENFREENOTERMINATE(current, current->len, newFree);
}

/* Enlarge free space at end of dks so caller can write 'addlen'
 * bytes.
 *
 * May grow more than requested to allow for fewer realloc calls over time.
 *
 * Note: this does not change the *length* of the dks string as returned
 * by DKS_LEN(), but only the free buffer space we have. */
DK_STATIC void DKS_INFOEXPANDBY(dksInfo *info, const size_t addlen) {
    /* If we already have enough free space to meet the 'addLen' request,
     * we don't need to add more free spacea
     *
     * This is our pre-optimized growth patterns in action. */
    if (info->free >= addlen) {
        return;
    }

    const size_t newlen = fibbufNextSizeBuffer(info->len + addlen);

    DKS_UPGRADEFREEALLOCATION(info, newlen);
}

DKS_TYPE *DKS_EXPANDBY(DKS_TYPE *s, size_t addlen) {
    /* Increase buffer space by a minimum of 'addlen' bytes
     * up to the next allocation size class. */
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    DKS_INFOEXPANDBY(&info, addlen);
    return info.buf;
}

DKS_TYPE *DKS_EXPANDBYEXACT(DKS_TYPE *s, const size_t addlen) {
    /* Increase buffer space by exactly 'addlen' without padding
     * the allocation with the additional allocation size class. */
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    /* If 'free' is greater than requested expansion, no need
     * for more free space. */
    if (info.free >= addlen) {
        return info.buf;
    }

    const size_t newlen = info.len + addlen;

    DKS_UPGRADEFREEALLOCATION(&info, newlen);
    return info.buf;
}

DKS_TYPE *DKS_REMOVEFREESPACE(DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

#if defined(DATAKIT_DKS_COMPACT) || defined(DATAKIT_DKS_COMPACT_HEADER)
    /* compact DKS ('mdsc') has no free space to remove! */
    return info.buf;
#else
    uint8_t headerSize = _dksHeaderSize(info.type);
    size_t reallocLen = headerSize + info.len + 1;

    info.start = zrealloc(info.start, reallocLen);
    info.buf = (DKS_TYPE *)(info.start + headerSize);

    /* Note: does *not* shrink the storage class of the dks.
     *       If free space was keeping DKS a size class larger,
     *       it will still remain in the larger size class,
     *       just with truncated free space now. */
    DKS_SETPREVIOUSINTEGERANDTYPE(info.buf, 0, info.type);

    return info.buf;
#endif
}

/* Return total size of allocation for dks string.
 * Includes all bytes used including:
 * 1) dks header before the pointer
 * 2) string data itself
 * 3) free buffer at the end (if any)
 * 4) the final null byte
 */
size_t DKS_ALLOCSIZE(DKS_TYPE *s) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    /*                    DKS_HEADER +      len +      free + NULL */
    return _dksHeaderSize(info.type) + info.len + info.free + 1;
}

/* Increment the dks length and decrement the free space by 'incr'.
 *
 * This function is used in order to fix the string length after the
 * user calls DKS_EXPANDBY(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using DKS_INCRLEN() and dksExpandBy() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * dks string without copying into an intermediate buffer:
 *
 * origLen = DKS_LEN(s);
 * s = DKS_EXPANDBY(s, BUFFER_SIZE);
 * nread = read(fd, s+origLen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * DKS_INCRLEN(s, nread);
 */
void DKS_INCRLEN(DKS_TYPE *s, ssize_t incr) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    /* If caller sets size > length of this entire DKS_TYPE,
     * bad things will happen.
     * But, the API returns no status for this function, so
     * we can't tell the caller their increase was bad.
     * Ideally, the caller already ran DKS_EXPANDBY() to
     * increase the DKS_TYPE before they increase the length here. */
    DKS_INCREASELENGTHBY(&info, incr);
}

/* Grow the dks to have the specified length. Bytes not part of
 * the original dks are set to zero.
 *
 * if requested length is smaller than current length, no-op. */
DKS_TYPE *DKS_GROWZERO(DKS_TYPE *s, const size_t len) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    const size_t curlen = info.len;

    if (len <= curlen) {
        return info.buf;
    }

    DKS_INFOEXPANDBY(&info, (len - curlen));

    memset(info.buf + curlen, 0, (len - curlen + 1)); /* includes '\0' byte */
    const size_t totlen = info.len + info.free;
    const size_t free = totlen - len;

    /* No Terminate because we just memset everything to zero above. */
    DKS_INFOUPDATELENFREENOTERMINATE(&info, len, free);

    return info.buf;
}

/* Append 't' to 's' */
DKS_TYPE *DKS_CATLEN(DKS_TYPE *s, const void *t, const size_t len) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    DKS_INFOEXPANDBY(&info, len);

    /* memmove because we don't know if 't' is inside 's' or not */
    memmove(info.buf + info.len, t, len);

    DKS_INCREASELENGTHBY(&info, len);

    return info.buf;
}

/* Text-based JSON manipulation helper */
DKS_TYPE *DKS_CATLEN_CHECK_COMMA(DKS_TYPE *s, const void *t, size_t len) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    if (unlikely(!len)) {
        return s;
    }

    DKS_INFOEXPANDBY(&info, len);

    /* If buffer has a comma as the previous element,
     * just overwrite it since whatever we are writing from 't'
     * doesn't want to have a comma before it. */
    if (info.buf[info.len - 1] == ',') {
        memmove(info.buf + info.len - 1, t, len);
        /* We extended the DKS by one less than requested,
         * so reflect in update length. */
        len--;
    } else {
        memmove(info.buf + info.len, t, len);
    }

    /* We may have actually zero change in length and free if we
     * just wrote something like '}' when the end of the buffer
     * was already ',' (we'd overwrite the ',' with '}' resulting
     * in zero net change to the string length). */
    if (len) {
        DKS_INCREASELENGTHBY(&info, len);
    }

    return info.buf;
}

DKS_TYPE *DKS_CATLEN_QUOTE_RAW(DKS_TYPE *s, const void *t, size_t len,
                               const uint8_t append) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    const size_t growWithQuotes = append ? 3 : 2;
    DKS_INFOEXPANDBY(&info, len + growWithQuotes);

    /* Write:
     *  - "INPUT"
     *  - OR -
     *  - "INPUT",
     */

    /* First quote at [0] offset */
    info.buf[info.len] = '"';

    /* Data at [0] + [1] offset */
    memmove(info.buf + info.len + 1, t, len);

    /* Final quote at [0] + len + [1] offset */
    info.buf[info.len + 1 + len] = '"';

    /* Optional after-quote append character at [0] + len + [1] + [1] offset */
    if (append) {
        info.buf[info.len + 1 + len + 1] = append;
    }

    /* Update the final written length... */
    DKS_INCREASELENGTHBY(&info, len + growWithQuotes);

    return info.buf;
}

DKS_TYPE *DKS_CATLEN_NOQUOTE_RAW(DKS_TYPE *s, const void *t, size_t len,
                                 const uint8_t append) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    const size_t growWith = append ? 1 : 0;
    DKS_INFOEXPANDBY(&info, len + growWith);

    /* Write:
     *  - INPUT
     *  - OR -
     *  - INPUT,
     */

    /* Data at [0] offset */
    memmove(info.buf + info.len, t, len);

    /* Optional after-quote append character at [0] + len offset */
    if (append) {
        info.buf[info.len + len] = append;
    }

    /* Update the final written length... */
    DKS_INCREASELENGTHBY(&info, len + growWith);

    return info.buf;
}

/* Prepend 't' to 's' */
DKS_TYPE *DKS_PREPENDLEN(DKS_TYPE *s, const void *t, const size_t len) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    /* Doesn't alter 'len', only grows extra space */
    DKS_INFOEXPANDBY(&info, len);

    /* Move existing buffer up so 't' will fit at the start */
    memmove(info.buf + len, info.buf, info.len);

    /* Now copy 't' to the start */
    memmove(info.buf, t, len);

    /* Declare content length has grown by 'len' */
    DKS_INCREASELENGTHBY(&info, len);

    return info.buf;
}

/* Append regular null terminated C string */
DKS_TYPE *DKS_CAT(DKS_TYPE *s, const char *t) {
    return DKS_CATLEN(s, t, strlen(t));
}

/* Append another dks */
DKS_TYPE *DKS_CATANOTHER(DKS_TYPE *s, const DKS_TYPE *t) {
    return DKS_CATLEN(s, t, DKS_LEN(t));
}

/* Overwrite dks with contents of 't' with 'newLength' bytes */
DKS_TYPE *DKS_COPYLEN(DKS_TYPE *s, const char *t, size_t newLength) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    size_t allocationSize = info.free + info.len;

    if (allocationSize < newLength) {
        DKS_INFOEXPANDBY(&info, newLength - info.len);
        allocationSize = info.free + info.len;
    }

    memcpy(info.buf, t, newLength);

    DKS_INFOUPDATELENFREE(&info, newLength, allocationSize - newLength);
    return info.buf;
}

/* Overwrite dks with contents of C string 't' */
DKS_TYPE *DKS_COPY(DKS_TYPE *s, const char *t) {
    return DKS_COPYLEN(s, t, strlen(t));
}

/* New dks string from int64_t value. Much faster than:
 * DKS_CATPRINTF(DKS_EMPTY(),"%lld\n", value);
 */
#define DKS_INT64STR_MAX 21
DKS_TYPE *DKS_FROMINT64(int64_t value) {
    char buf[DKS_INT64STR_MAX] = {0};
    const int len = StrInt64ToBuf(buf, sizeof(buf), value);

    return DKS_NEWLEN(buf, len);
}

/* Like DKS_CATPRINTF() but gets va_list instead of being variadic. */
DKS_TYPE *DKS_CATVPRINTF(DKS_TYPE *s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[2048];
    char *buf = staticbuf;
    char *t;
    size_t buflen = strlen(fmt) * 2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = zmalloc(buflen);
    } else {
        buflen = sizeof(staticbuf);
    }

    /* If write fails, double buffer size and try again */
    while (true) {
        buf[buflen - 2] = '\0';
        va_copy(cpy, ap);
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen - 2] != '\0') {
            if (buf != staticbuf) {
                zfree(buf);
            }

            buflen *= 2;
            buf = zmalloc(buflen);
            continue;
        }

        break;
    }

    /* Finally concat the obtained string to the dks string and return it. */
    t = DKS_CAT(s, buf);
    if (buf != staticbuf) {
        zfree(buf);
    }

    return t;
}

/* Append a string obtained using format specifiers.
 *
 * Example:
 *
 * s = DKS_NEW("Sum is: ");
 * s = DKS_CATPRINTF(s,"%d+%d = %d",a,b,a+b).
 *
 * s = DKS_CATPRINTF(DKS_EMPTY(), "%s %u ...", args);
 */
DKS_TYPE *DKS_CATPRINTF(DKS_TYPE *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DKS_TYPE *result = DKS_CATVPRINTF(s, fmt, ap);
    va_end(ap);
    return result;
}

#define GROW_FREE(info, limit)                                                 \
    do {                                                                       \
        if ((info)->free < (limit)) {                                          \
            DKS_INFOEXPANDBY(info, limit);                                     \
        }                                                                      \
    } while (0)

/* Native mock version of DKS_CATPRINTF but parse the format string manually.
 *
 * %b - databox bytes
 * %B - databox any value
 * %s - C String
 * %S - dks string
 * %i - int32_t
 * %I - int64_t
 * %u - uint32_t
 * %U - uint64_t
 * %% - Verbatim "%" character.
 */
DKS_TYPE *DKS_CATFMT(DKS_TYPE *s, char const *fmt, ...) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    size_t initlen = info.len;
    va_list ap;

    va_start(ap, fmt);
    const char *f = fmt; /* format specifier processing position */
    size_t i = initlen;  /* current write offset */
    while (*f) {
        const databox *box;
        char *str;
        size_t l;
        int64_t num;
        uint64_t unum;
        uint8_t writerBuf[32];

        /* verify free space exists */
        GROW_FREE(&info, 1);

        if (*f == '%') {
            /* jump over '%' */
            f++;

            uint8_t next = *f;

            /* reached end of format string */
            if (!next) {
                break;
            }

            switch (next) {
            case 'b':
                box = va_arg(ap, databox *);
                l = databoxLen(box);
                GROW_FREE(&info, l);
                memcpy(info.buf + i, databoxBytes(box), l);
                i = DKS_INCREASELENGTHBYNOTERMINATE(&info, l);
                break;
            case 'B': {
                uint8_t *useBuf = writerBuf;
                box = va_arg(ap, const databox *);
                switch (box->type) {
                case DATABOX_BYTES:
                case DATABOX_BYTES_EMBED:
                    l = databoxLen(box);
                    useBuf = (uint8_t *)databoxBytes(box);
                    break;
                case DATABOX_SIGNED_64:
                    l = StrInt64ToBuf(writerBuf, sizeof(writerBuf),
                                      box->data.i);
                    break;
                case DATABOX_UNSIGNED_64:
                    l = StrUInt64ToBuf(writerBuf, sizeof(writerBuf),
                                       box->data.i);
                    break;
                case DATABOX_FLOAT_32:
                    l = StrDoubleFormatToBufNice(writerBuf, sizeof(writerBuf),
                                                 box->data.f32);
                    break;
                case DATABOX_DOUBLE_64:
                    l = StrDoubleFormatToBufNice(writerBuf, sizeof(writerBuf),
                                                 box->data.d64);
                    break;
                default:
                    assert(NULL && "Unsupported box data!");
                    l = 0; /* avoid warning when NDEBUG compile */
                }

                GROW_FREE(&info, l);
                memcpy(info.buf + i, useBuf, l);
                i = DKS_INCREASELENGTHBYNOTERMINATE(&info, l);
                break;
            }
            case 's':
            case 'S':
                str = va_arg(ap, char *);
                l = (next == 's') ? strlen(str) : DKS_LEN(str);
                GROW_FREE(&info, l);
                memcpy(info.buf + i, str, l);
                i = DKS_INCREASELENGTHBYNOTERMINATE(&info, l);
                break;
            case 'i':
            case 'I':
                if (next == 'i') {
                    num = va_arg(ap, int32_t);
                } else {
                    num = va_arg(ap, int64_t);
                }

                l = StrInt64ToBuf(writerBuf, sizeof(writerBuf), num);
                GROW_FREE(&info, l);
                memcpy(info.buf + i, writerBuf, l);
                i = DKS_INCREASELENGTHBYNOTERMINATE(&info, l);

                break;
            case 'u':
            case 'U':
                if (next == 'u') {
                    unum = va_arg(ap, uint32_t);
                } else {
                    unum = va_arg(ap, uint64_t);
                }

                l = StrUInt64ToBuf(writerBuf, sizeof(writerBuf), unum);
                GROW_FREE(&info, l);
                memcpy(info.buf + i, writerBuf, l);
                i = DKS_INCREASELENGTHBYNOTERMINATE(&info, l);

                break;
            default: /* Handle %% and generally %<unknown>. */
                info.buf[i++] = next;
                DKS_INCREASELENGTHBYNOTERMINATE(&info, 1);
                break;
            }
        } else {
            info.buf[i++] = *f;
            DKS_INCREASELENGTHBYNOTERMINATE(&info, 1);
        }

        f++;
    }

    va_end(ap);

    assert(info.len == i);
    info.buf[i] = '\0';
    return info.buf;
}

/* Remove characters from left and from right if characters in 'cset'
 *
 * Example:
 *
 * s = DKS_NEW("AA...AA.a.aa.aHelloWorld     :::");
 * s = DKS_TRIM(s,"A. a:");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
DKS_TYPE *DKS_TRIM(DKS_TYPE *s, const char *cset) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = info.buf;
    ep = end = info.buf + info.len - 1;

    while (sp <= end && strchr(cset, *sp)) {
        sp++;
    }

    while (ep > sp && strchr(cset, *ep)) {
        ep--;
    }

    len = (sp > ep) ? 0 : ((ep - sp) + 1);

    if (info.buf != sp) {
        memmove(info.buf, sp, len);
    }

    DKS_INFOUPDATELENFREE(&info, len, info.free + (info.len - len));
    return info.buf;
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Example:
 *
 * s = DKS_NEW("Hello World");
 * DKS_RANGE(s,1,-1); => "ello World"
 */
void DKS_RANGE(DKS_TYPE *s, ssize_t start, ssize_t end) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    const size_t len = info.len;
    if (len == 0) {
        return;
    }

    if (start < 0) {
        start = len + start;
        if (start < 0) {
            start = 0;
        }
    }

    if (end < 0) {
        end = len + end;
        if (end < 0) {
            end = 0;
        }
    }

    size_t newlen = (start > end) ? 0 : (end - start) + 1;
    if (newlen != 0) {
        if (start >= (ssize_t)len) {
            newlen = 0;
        } else if (end >= (ssize_t)len) {
            end = len - 1;
            newlen = (start > end) ? 0 : (end - start) + 1;
        }
    } else {
        start = 0;
    }

    if (start && newlen) {
        memmove(info.buf, info.buf + start, newlen);
    }

    DKS_INFOUPDATELENFREE(&info, newlen, info.free + (info.len - newlen));
}

/* Convert string to only be 'length' characters from 'start' offset */
void DKS_SUBSTR(DKS_TYPE *s, size_t start, size_t length) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    const size_t len = info.len;
    /* If start position is beyond the end of the string, don't do anything. */
    if (start > len) {
        return;
    }

    /* If length is more than our string extent, limit size to end of string. */
    if ((start + length) > len) {
        length = (len - start);
    }

    /* Move start position to beginning of string for 'length' */
    memmove(info.buf, info.buf + start, length);

    DKS_INFOUPDATELENFREE(&info, length, info.free + (info.len - length));
}

/* Convert string to only be 'length' utf-8 characters from 'start' offset */
void DKS_SUBSTR_UTF8(DKS_TYPE *s, size_t start, size_t length) {
    dksInfo info;
    DKS_BASE(s, &info);
    s = NULL;

    const size_t len = info.len;
    const uint8_t *str = (uint8_t *)info.buf;

    /* If requested start position is beyond the end of the string,
     * don't do anything. */
    if (start > len) {
        return;
    }

    const size_t startOffset = StrLenUtf8CountBytes(str, len, start);

    /* If calculated start offset is beyond the end of the string,
     * don't do anything. */
    if (startOffset > len) {
        return;
    }

    size_t extentBytes =
        StrLenUtf8CountBytes(str + startOffset, len - startOffset, length);

    /* Note: 'StrLenUtf8CountBytes' won't return a too-big extent,
     * so we don't need an additional "check if extent is too big for string"
     * check here. */

    /* Move start position to beginning of string for 'length' */
    memmove(info.buf, info.buf + startOffset, extentBytes);

    DKS_INFOUPDATELENFREE(&info, extentBytes,
                          info.free + (info.len - extentBytes));
}

/* Apply tolower() to every character */
void DKS_TOLOWER(DKS_TYPE *s) {
    size_t len = DKS_LEN(s);

    for (size_t j = 0; j < len; j++) {
        s[j] = StrTolower(s[j]);
    }
}

/* Apply toupper() to every character */
void DKS_TOUPPER(DKS_TYPE *s) {
    size_t len = DKS_LEN(s);

    for (size_t j = 0; j < len; j++) {
        s[j] = StrToupper(s[j]);
    }
}

/* Compare two dks strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If string prefixes are equal, the longer string is greater than
 * the smaller string. */
int DKS_CMP(const DKS_TYPE *s1, const DKS_TYPE *s2) {
    size_t l1 = DKS_LEN(s1);
    size_t l2 = DKS_LEN(s2);
    size_t minlen = (l1 < l2) ? l1 : l2;
    int cmp = memcmp(s1, s2, minlen);

    if (cmp == 0) {
        return l1 - l2;
    }

    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of dks strings is returned. *count is set
 * to the number of tokens returned.
 *
 * Note 'sep' is able to split a string using
 * a multi-character separator. For example
 * DKS_SPLIT("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 */
DKS_TYPE **DKS_SPLITLEN_MAX(const void *const s_, const size_t len,
                            const void *const sep_, const size_t sepLen,
                            size_t *const count, size_t maxCount) {
    const uint8_t *const s = s_;
    const uint8_t *const sep = sep_;
    size_t slots = maxCount ?: 4;
    size_t elements = 0;
    size_t start = 0;

    if (!len || !sepLen) {
        *count = 0;
        return NULL;
    }

    DKS_TYPE **tokens = zcalloc(slots, sizeof(*tokens));

    for (size_t j = 0; j < (len - (sepLen - 1)); j++) {
        /* If we grabbed max items, stop parsing. */
        if (maxCount && elements == slots) {
            break;
        }

        /* make sure there is room for the next element and the final one */
        const size_t maximumSlotsNeeded = maxCount ?: (elements + 2);
        if (slots < maximumSlotsNeeded) {
            /* Summon the power of EXPONENTIAL GROWTH! */
            slots *= 2;
            tokens = zrealloc(tokens, sizeof(*tokens) * slots);
        }

        /* search if s[j] is at the separator */
        if (memcmp(&s[j], sep, sepLen) == 0) {
            /* We assume 'DKS_NEWLEN' always returns a usable value (i.e. we
             * aren't checking or allowing for out of memory here, you should
             * handle that elsewhere or use something other than garbage C that
             * expects to propagate out of memory errors up through sub-layers
             * to top-layers of calls) */
            tokens[elements] = DKS_NEWLEN(&s[start], j - start);

            /* We found an element! Increment our found element count. */
            elements++;

            /* recorded string position after token so we can record the FINAL
             * string after the LAST 'sep' is detected (after this entire for
             * loop) */
            start = j + sepLen;

            /* jump over 'sep' for next string postion check */
            j += sepLen - 1;
        }
    }

    /* IF maxCount, only add last element if we're not at maxCount yet;
     * else: unconditionally add last element */
    if (maxCount && elements >= maxCount) {
        goto finalize;
    }

    assert(elements < slots);

    /* Add the final element. We know there's room for it. */
    if (start < len) {
        tokens[elements] = DKS_NEWLEN(&s[start], len - start);
        elements++;
    }

    /* fallthrough to return */

finalize:
    *count = elements;
    return tokens;
}

DKS_TYPE **DKS_SPLITLEN(const void *const s_, const size_t len,
                        const void *const sep_, const size_t sepLen,
                        size_t *const count) {
    /* Request splitlen with unlimited maximum return count */
    return DKS_SPLITLEN_MAX(s_, len, sep_, sepLen, count, 0);
}

/* Free result returned by DKS_SPLITLEN(), or do nothing if 'tokens' is NULL. */
void DKS_SPLITLENFREE(DKS_TYPE **tokens, size_t count) {
    if (tokens) {
        while (count--) {
            DKS_FREE(tokens[count]);
        }

        zfree(tokens);
    }
}

/* Append to the dks string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>". */
#include <ctype.h> /* for isprint() only */
DKS_TYPE *DKS_CATREPR(DKS_TYPE *s, const char *p, size_t len) {
    s = DKS_CATLEN(s, "\"", 1);
    while (len--) {
        switch (*p) {
        case '\\':
        case '"':
            s = DKS_CATPRINTF(s, "\\%c", *p);
            break;
        case '\n':
            s = DKS_CATLEN(s, "\\n", 2);
            break;
        case '\r':
            s = DKS_CATLEN(s, "\\r", 2);
            break;
        case '\t':
            s = DKS_CATLEN(s, "\\t", 2);
            break;
        case '\a':
            s = DKS_CATLEN(s, "\\a", 2);
            break;
        case '\b':
            s = DKS_CATLEN(s, "\\b", 2);
            break;
        default:
            if (isprint(*p)) {
                s = DKS_CATPRINTF(s, "%c", *p);
            } else {
                s = DKS_CATPRINTF(s, "\\x%02x", (unsigned char)*p);
            }

            break;
        }

        p++;
    }

    return DKS_CATLEN(s, "\"", 1);
}

/* Return true if input is hex digit */
DK_STATIC bool DKS_ISHEXDIGIT(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

DK_STATIC int DKS_HEXDIGITTOINT(char c) {
    switch (c) {
    case '0':
        return 0;
    case '1':
        return 1;
    case '2':
        return 2;
    case '3':
        return 3;
    case '4':
        return 4;
    case '5':
        return 5;
    case '6':
        return 6;
    case '7':
        return 7;
    case '8':
        return 8;
    case '9':
        return 9;
    case 'a':
    case 'A':
        return 10;
    case 'b':
    case 'B':
        return 11;
    case 'c':
    case 'C':
        return 12;
    case 'd':
    case 'D':
        return 13;
    case 'e':
    case 'E':
        return 14;
    case 'f':
    case 'F':
        return 15;
    default:
        return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-like form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of dks is returned.
 *
 * The caller should free the resulting array of dks strings with
 * DKS_FREESPLITRES().
 *
 * Note that DKS_CATREPR() is able to convert back a string into
 * a quoted string in the same format DKS_SPLITARGS() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
DKS_TYPE **DKS_SPLITARGS(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while (1) {
        /* skip blanks */
        while (*p && StrIsspace(*p)) {
            p++;
        }

        if (*p) {
            /* get a token */
            int inq = 0;  /* set to 1 if we are in "quotes" */
            int insq = 0; /* set to 1 if we are in 'single quotes' */
            int done = 0;

            if (current == NULL) {
                current = DKS_EMPTY();
            }

            while (!done) {
                if (inq) {
                    if (*p == '\\' && *(p + 1) == 'x' &&
                        DKS_ISHEXDIGIT(*(p + 2)) && DKS_ISHEXDIGIT(*(p + 3))) {
                        unsigned char byte;

                        byte = (DKS_HEXDIGITTOINT(*(p + 2)) * 16) +
                               DKS_HEXDIGITTOINT(*(p + 3));
                        current = DKS_CATLEN(current, (char *)&byte, 1);
                        p += 3;
                    } else if (*p == '\\' && *(p + 1)) {
                        char c;

                        p++;
                        switch (*p) {
                        case 'n':
                            c = '\n';
                            break;
                        case 'r':
                            c = '\r';
                            break;
                        case 't':
                            c = '\t';
                            break;
                        case 'b':
                            c = '\b';
                            break;
                        case 'a':
                            c = '\a';
                            break;
                        default:
                            c = *p;
                            break;
                        }

                        current = DKS_CATLEN(current, &c, 1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !StrIsspace(*(p + 1))) {
                            goto err;
                        }

                        done = 1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = DKS_CATLEN(current, p, 1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p + 1) == '\'') {
                        p++;
                        current = DKS_CATLEN(current, "'", 1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p + 1) && !StrIsspace(*(p + 1))) {
                            goto err;
                        }

                        done = 1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = DKS_CATLEN(current, p, 1);
                    }
                } else {
                    switch (*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done = 1;
                        break;
                    case '"':
                        inq = 1;
                        break;
                    case '\'':
                        insq = 1;
                        break;
                    default:
                        current = DKS_CATLEN(current, p, 1);
                        break;
                    }
                }
                if (*p) {
                    p++;
                }
            }
            /* add the token to the vector */
            vector = zrealloc(vector, ((*argc) + 1) * sizeof(char *));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) {
                vector = zmalloc(sizeof(void *));
            }

            return vector;
        }
    }

err:
    while ((*argc)--) {
        DKS_FREE(vector[*argc]);
    }

    zfree(vector);
    DKS_FREE(current);
    *argc = 0;
    return NULL;
}

/* In-place substitute all the occurrences in 'from' to corresponding 'to'.
 *
 * DKS_MAPCHARS(mystring, "ho", "01", 2) * turns "hello" into "0ell1". */
void DKS_MAPCHARS(DKS_TYPE *s, const char *from, const char *to,
                  size_t setlen) {
    size_t l = DKS_LEN(s);

    for (size_t j = 0; j < l; j++) {
        for (size_t i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as dks string. */
DKS_TYPE *DKS_JOIN(char **argv, int argc, char *sep) {
    DKS_TYPE *join = DKS_EMPTY();

    for (int j = 0; j < argc; j++) {
        join = DKS_CAT(join, argv[j]);
        if (j != argc - 1) {
            join = DKS_CAT(join, sep);
        }
    }

    return join;
}

/* Properly format an IP:Port based on IPv4 vs. IPv6 requirements. */
DKS_TYPE *DKS_FORMATIP(char *ip, int port) {
    const bool isIPv6 = !!strchr(ip, ':');

    if (port >= 0) {
        return DKS_CATFMT(DKS_EMPTY(), isIPv6 ? "[%s]:%i" : "%s:%i", ip, port);
    } else {
        return DKS_CATFMT(DKS_EMPTY(), isIPv6 ? "[%s]" : "%s", ip);
    }
}

/* Given the filename, return the absolute path as dks string, or NULL
 * if it fails for some reason. Note that "filename" may be an absolute path
 * already, this will be detected and handled correctly.
 *
 * The function does not try to normalize everything, but only the obvious
 * case of one or more "../" appearning at the start of "filename"
 * relative path. */
DKS_TYPE *DKS_GETABSOLUTEPATH(char *filename) {
    char cwd[1024] = {0};
    DKS_TYPE *abspath;
    DKS_TYPE *relpath = DKS_NEW(filename);

    relpath = DKS_TRIM(relpath, " \r\n\t");
    if (relpath[0] == '/') {
        return relpath; /* Path is already absolute. */
    }

    /* If path is relative, join cwd and relative path. */
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        DKS_FREE(relpath);
        return NULL;
    }

    abspath = DKS_NEW(cwd);
    if (DKS_LEN(abspath) && abspath[DKS_LEN(abspath) - 1] != '/') {
        abspath = DKS_CAT(abspath, "/");
    }

    /* At this point we have the current path always ending with "/", and
     * the trimmed relative path. Try to normalize the obvious case of
     * trailing ../ elements at the start of the path.
     *
     * For every "../" we find in the filename, we remove it and also remove
     * the last element of the cwd, unless the current cwd is "/". */
    while (DKS_LEN(relpath) >= 3 && relpath[0] == '.' && relpath[1] == '.' &&
           relpath[2] == '/') {
        DKS_RANGE(relpath, 3, -1);
        if (DKS_LEN(abspath) > 1) {
            char *p = abspath + DKS_LEN(abspath) - 2;
            int trimlen = 1;

            while (*p != '/') {
                p--;
                trimlen++;
            }

            DKS_RANGE(abspath, 0, -(trimlen + 1));
        }
    }

    /* Finally glue the two parts together. */
    abspath = DKS_CATANOTHER(abspath, relpath);
    DKS_FREE(relpath);
    return abspath;
}

size_t DKS_LENUTF8(DKS_TYPE *s) {
    return StrLenUtf8(s, DKS_LEN(s));
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#if defined(DATAKIT_TEST)
#define testCond(descr, _c, _target, _fmt)                                     \
    _testCond(descr, _c, _target, _fmt, ==)
#define testCondGte(descr, _c, _target, _fmt)                                  \
    _testCond(descr, _c, _target, _fmt, >=)
#define _testCond(descr, _c, _target, _fmt, _compare)                          \
    do {                                                                       \
        __testNum++;                                                           \
        printf("%d - %s: ", __testNum, descr);                                 \
        if ((_c)_compare(_target)) {                                           \
            printf("PASSED\n");                                                \
        } else {                                                               \
            printf("FAILED! " _fmt " != " _fmt "\n", _c, _target);             \
            if (hardStop) {                                                    \
                assert(NULL);                                                  \
            }                                                                  \
            __failed_tests++;                                                  \
        }                                                                      \
    } while (0)

#define testReport()                                                           \
    do {                                                                       \
        printf("%d tests, %d passed, %d failed\n", __testNum,                  \
               __testNum - __failed_tests, __failed_tests);                    \
        if (__failed_tests) {                                                  \
            printf("=== WARNING === We have failed tests here...\n");          \
            if (hardStop) {                                                    \
                printf("Note: this was an 'mdsc' test and some tests around "  \
                       "size classes don't pass under 'mdsc' testing.\n");     \
                printf("If 'mds' tests clean and 'mdsc' has size errors, "     \
                       "it's okay.\n");                                        \
            }                                                                  \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#include "limits.h"
#include <fcntl.h>
#include <stdio.h>

struct testLimit {
    dksType startType;
    uint64_t maxContentSize;
    int32_t incrby;
    dksType newType;
};

DK_STATIC char *DKS_TYPENAME(dksType type) {
    switch (type) {
    case DKS_8:
        return "DKS_8";
    case DKS_16:
        return "DKS_16";
    case DKS_24:
        return "DKS_24";
    case DKS_32:
        return "DKS_32";
    case DKS_40:
        return "DKS_40";
    case DKS_48:
        return "DKS_48";
    }

    assert(NULL);
    __builtin_unreachable();
}

DK_STATIC void DKS_RANDBYTES(uint8_t *buf, size_t len) {
    size_t total = 0;
    size_t chunkSize = (1024 * 1024 * 64); /* max 64 MB random data */

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
}

#define UNUSED(x) (void)(x)
int DKS_TEST(int argc, char *argv[]) {
    UNUSED(argv);

    bool bigTests = true;
    bool hardStop = true;

    for (int i = 2; i < argc; i++) {
        /* We need nostop because DKS_COMPACT fails
         * the availability tests (because it never has any available space)
         * and it fails the min size tests since it has a different size
         * range than mds (and we don't have per-impl test ranges yet) */
        if (!strcmp(argv[i], "nostop") || !strcmp(argv[i], "mdsc")) {
            hardStop = false;
        } else if (!strcmp(argv[i], "nobig")) {
            bigTests = false;
        }
    }

    int __failed_tests = 0;
    int __testNum = 0;

    {
        DKS_TYPE *x = DKS_NEWLEN("foobar", 6);
        testCond("New length string", DKS_LEN(x), (size_t)6, "%zu");
        testCond("New length string, avail", DKS_AVAIL(x), (size_t)0, "%zu");
        testCond("New length string, get value", memcmp(x, "foobar\0", 7), 0,
                 "%d");
        DKS_FREE(x);

        x = DKS_NEWLEN(NULL, 6);
        testCond("New length empty string", DKS_LEN(x), (size_t)6, "%zu");
        testCond("New length empty string, avail", DKS_AVAIL(x), (size_t)0,
                 "%zu");
        testCond("New length empty string, get value",
                 memcmp(x, "\0\0\0\0\0\0\0", 7), 0, "%d");
        DKS_FREE(x);

        x = DKS_NEWLEN("abc", 0);
        testCond("New zero empty string", DKS_LEN(x), (size_t)0, "%zu");
        testCond("New zero empty string, avail", DKS_AVAIL(x), (size_t)0,
                 "%zu");
        testCond("New zero empty string, get value", memcmp(x, "\0", 1), 0,
                 "%d");
        DKS_FREE(x);
    }

    {
        DKS_TYPE *x = DKS_NEW("foo");
        DKS_TYPE *y;

        testCond("New string and obtain the length", DKS_LEN(x), (size_t)3,
                 "%zu");
        testCond("New string and obtain the length", memcmp(x, "foo\0", 4), 0,
                 "%d");
        DKS_FREE(x);

        x = DKS_NEWLEN("foo", 2);
        testCond("New string with specified length", DKS_LEN(x), (size_t)2,
                 "%zu");
        testCond("New string with specified length", memcmp(x, "fo\0", 3), 0,
                 "%d");

        x = DKS_CAT(x, "bar");
        testCond("Strings concatenation", DKS_LEN(x), (size_t)5, "%zu");
        testCond("Strings concatenation", memcmp(x, "fobar\0", 6), 0, "%d");

        x = DKS_COPY(x, "a");
        testCond("DKS_COPY() against an originally longer string", DKS_LEN(x),
                 (size_t)1, "%zu");
        testCond("DKS_COPY() against an originally longer string",
                 memcmp(x, "a\0", 2), 0, "%d");

        x = DKS_COPY(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        testCond("DKS_COPY() against an originally shorter string", DKS_LEN(x),
                 (size_t)33, "%zu");
        testCond("DKS_COPY() against an originally shorter string",
                 memcmp(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0", 33), 0, "%d");

        DKS_FREE(x);
        x = DKS_CATPRINTF(DKS_EMPTY(), "%d", 123);
        testCond("DKS_CATPRINTF() seems working in the base case", DKS_LEN(x),
                 (size_t)3, "%zu");
        testCond("DKS_CATPRINTF() seems working in the base case",
                 memcmp(x, "123\0", 4), 0, "%d");

        DKS_FREE(x);

        /* Test no crash because bad format specifiers */
        x = DKS_NEW("--");
        x = DKS_CATFMT(x, "Hello % World %,%--", "Hi!", INT64_MIN, INT64_MAX);
        DKS_FREE(x);

        /* Test no crash because bad format specifiers at end */
        x = DKS_NEW("--");
        x = DKS_CATFMT(x, "Hello % World %,%--%", "Hi!", INT64_MIN, INT64_MAX);
        DKS_FREE(x);

        x = DKS_NEW("--");
        x = DKS_CATFMT(x, "Hello %s World %I,%I--", "Hi!", INT64_MIN,
                       INT64_MAX);
        testCond("DKS_CATFMT() length matches expected size", DKS_LEN(x),
                 (size_t)60, "%zu");
        testCond("DKS_CATFMT() content matches expected bytes",
                 memcmp(x,
                        "--Hello Hi! World -9223372036854775808,"
                        "9223372036854775807--",
                        60),
                 0, "%d");

        DKS_FREE(x);
        x = DKS_NEW("--");
        x = DKS_CATFMT(x, "%u,%U--", UINT32_MAX, UINT64_MAX);
        testCond("DKS_CATFMT() seems working with unsigned numbers", DKS_LEN(x),
                 (size_t)35, "%zu");
        testCond("DKS_CATFMT() seems working with unsigned numbers",
                 memcmp(x, "--4294967295,18446744073709551615--", 35), 0, "%d");

        DKS_FREE(x);
        x = DKS_NEW(" x ");
        DKS_TRIM(x, " x");
        testCond("DKS_TRIM() works when all chars match", DKS_LEN(x), (size_t)0,
                 "%zu");

        DKS_FREE(x);
        x = DKS_NEW(" x ");
        DKS_TRIM(x, " ");
        testCond("DKS_TRIM() works when a single char remains", DKS_LEN(x),
                 (size_t)1, "%zu");
        testCond("DKS_TRIM() works when a single char remains", x[0], 'x',
                 "%c");

        DKS_FREE(x);
        x = DKS_NEW("xxciaoyyy");
        DKS_TRIM(x, "xy");
        testCond("DKS_TRIM() correctly trims characters", DKS_LEN(x), (size_t)4,
                 "%zu");
        testCond("DKS_TRIM() correctly trims characters",
                 memcmp(x, "ciao\0", 5), 0, "%d");

        y = DKS_DUP(x);
        DKS_RANGE(y, 1, 1);
        testCond("DKS_RANGE(...,1,1)", DKS_LEN(y), (size_t)1, "%zu");
        testCond("DKS_RANGE(...,1,1)", memcmp(y, "i\0", 2), 0, "%d");

        DKS_FREE(y);
        y = DKS_DUP(x);
        DKS_RANGE(y, 1, -1);
        testCond("DKS_RANGE(...,1,-1)", DKS_LEN(y), (size_t)3, "%zu");
        testCond("DKS_RANGE(...,1,-1)", memcmp(y, "iao\0", 4), 0, "%d");

        DKS_FREE(y);
        y = DKS_DUP(x);
        DKS_RANGE(y, -2, -1);
        testCond("DKS_RANGE(...,-2,-1)", DKS_LEN(y), (size_t)2, "%zu");
        testCond("DKS_RANGE(...,-2,-1)", memcmp(y, "ao\0", 3), 0, "%d");

        DKS_FREE(y);
        y = DKS_DUP(x);
        DKS_RANGE(y, 2, 1);
        testCond("DKS_RANGE(...,2,1)", DKS_LEN(y), (size_t)0, "%zu");
        testCond("DKS_RANGE(...,2,1)", memcmp(y, "\0", 1), 0, "%d");

        DKS_FREE(y);
        y = DKS_DUP(x);
        DKS_RANGE(y, 1, 100);
        testCond("DKS_RANGE(...,1,100)", DKS_LEN(y), (size_t)3, "%zu");
        testCond("DKS_RANGE(...,1,100)", memcmp(y, "iao\0", 4), 0, "%d");

        DKS_FREE(y);
        y = DKS_DUP(x);
        DKS_RANGE(y, 100, 100);
        testCond("DKS_RANGE(...,100,100)", DKS_LEN(y), (size_t)0, "%zu");
        testCond("DKS_RANGE(...,100,100)", memcmp(y, "\0", 1), 0, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
        x = DKS_NEW("foo");
        y = DKS_NEW("foa");
        testCond("DKS_CMP(foo,foa)", DKS_CMP(x, y), 14, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
        x = DKS_NEW("bar");
        y = DKS_NEW("bar");
        testCond("DKS_CMP(bar,bar)", DKS_CMP(x, y), 0, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
        x = DKS_NEW("aar");
        y = DKS_NEW("bar");
        testCond("DKS_CMP(aar,bar)", DKS_CMP(x, y), -1, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
        x = DKS_NEWLEN("\a\n\0foo\r", 7);
        y = DKS_CATREPR(DKS_EMPTY(), x, DKS_LEN(x));
        testCond("DKS_CATREPR(...data...)",
                 memcmp(y, "\"\\a\\n\\x00foo\\r\"", 15), 0, "%d");
        DKS_FREE(y);
        DKS_FREE(x);
        {
            x = DKS_NEW("0");
            testCond("DKS_NEW() free/len buffers", DKS_LEN(x), (size_t)1,
                     "%zu");
            testCond("DKS_NEW() free/len buffers", DKS_AVAIL(x), (size_t)0,
                     "%zu");
            x = DKS_EXPANDBY(x, 1);
            testCond("DKS_EXPANDBY()", DKS_LEN(x), (size_t)1, "%zu");
/* We can't do DKS_AVAIL() testing in compact mode. */
#if !defined(DATAKIT_DKS_COMPACT)
            /* this '44' comes from jebufSizeAllocation() rounding up
             * our already rounded-up fibbufNextSizeBuffer() request. */
            testCond("DKS_EXPANDBY()", DKS_AVAIL(x), (size_t)44, "%zu");
            uint32_t origFree = DKS_AVAIL(x);
            x[1] = '1';
            DKS_INCRLEN(x, 1);
            testCond("DKS_INCRLEN() -- content", x[0], '0', "%c");
            testCond("DKS_INCRLEN() -- content", x[1], '1', "%c");
            testCond("DKS_INCRLEN() -- len", DKS_LEN(x), (size_t)2, "%zu");
            testCond("DKS_INCRLEN() -- free", DKS_AVAIL(x),
                     (size_t)origFree - 1, "%zu");
#endif
            DKS_FREE(x);
        }

        {
            x = DKS_NEW("abcdefhello therefedcba");

            DKS_SUBSTR(x, 6, 11);
            testCond("DKS_SUBSTR() - middle len ", (int)DKS_LEN(x), 11, "%d");
            testCond("DKS_LENUTF8() - middle len ", (int)DKS_LENUTF8(x), 11,
                     "%d");
            testCond("DKS_SUBSTR() - middle contents",
                     memcmp(x, "hello there", strlen("hello there") + 1), 0,
                     "%d");

            DKS_SUBSTR(x, 100, 5);
            testCond("DKS_SUBSTR() - too big len ", (int)DKS_LEN(x), 11, "%d");
            testCond("DKS_LENUTF8() - too big len ", (int)DKS_LENUTF8(x), 11,
                     "%d");
            testCond("DKS_SUBSTR() - too big contents",
                     memcmp(x, "hello there", strlen("hello there") + 1), 0,
                     "%d");

            DKS_SUBSTR(x, 3, 100);
            testCond("DKS_SUBSTR() - run off end len ", (int)DKS_LEN(x), 11 - 3,
                     "%d");
            testCond("DKS_LENUTF8() - run off end len", (int)DKS_LENUTF8(x),
                     11 - 3, "%d");
            testCond("DKS_SUBSTR() - run off end contents",
                     memcmp(x, "lo there", strlen("lo there") + 1), 0, "%d");

            DKS_FREE(x);
        }

        {
#define _TU8MIDDLES                                                            \
    "\xF0\x9F\x98\x81"                                                         \
    "\xF0\x9F\x98\x82"                                                         \
    "\xF0\x9F\x98\x83"                                                         \
    "\xF0\x9F\x98\x84"                                                         \
    "\xF0\x9F\x98\x85"
            x = DKS_NEW("abcdefhello " _TU8MIDDLES "fedcba");
            testCond("DKS_LENUTF8() - count characters", (int)DKS_LENUTF8(x),
                     23, "%d");
            testCond("DKS_LEN() - count bytes", (int)DKS_LEN(x), 38, "%d");

            DKS_SUBSTR_UTF8(x, 6, 11);
            testCond("DKS_LENUTF8() - count characters", (int)DKS_LENUTF8(x),
                     11, "%d");
            testCond("DKS_SUBSTR() - middle len ", (int)DKS_LEN(x), 26, "%d");
            testCond("DKS_SUBSTR() - middle contents",
                     memcmp(x, "hello " _TU8MIDDLES,
                            strlen("hello " _TU8MIDDLES) + 1),
                     0, "%d");

            DKS_SUBSTR_UTF8(x, 100, 5);
            testCond("DKS_SUBSTR() - too big len ", (int)DKS_LEN(x), 26, "%d");
            testCond("DKS_LENUTF8() - count characters", (int)DKS_LENUTF8(x),
                     11, "%d");
            testCond("DKS_SUBSTR() - too big contents",
                     memcmp(x, "hello " _TU8MIDDLES,
                            strlen("hello " _TU8MIDDLES) + 1),
                     0, "%d");

            DKS_SUBSTR_UTF8(x, 3, 100);
            testCond("DKS_SUBSTR() - run off end len ", (int)DKS_LEN(x), 26 - 3,
                     "%d");
            testCond("DKS_LENUTF8() - count characters", (int)DKS_LENUTF8(x),
                     11 - 3, "%d");
            testCond(
                "DKS_SUBSTR() - run off end contents",
                memcmp(x, "lo " _TU8MIDDLES, strlen("lo " _TU8MIDDLES) + 1), 0,
                "%d");

            DKS_FREE(x);
        }

        x = DKS_FROMINT64(16384);
        y = DKS_FROMINT64(-2);
        testCond("Verify int64 +", memcmp(x, "16384", 5), 0, "%d");
        testCond("Verify int64 -", memcmp(y, "-2", 2), 0, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
        x = DKS_EMPTY();
        x = DKS_CATFMT(x,
                       "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s "
                       "db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U "
                       "obl=%u oll=%u omem=%u events=%s cmd=%s",
                       0ULL, "helo", 2, "hello2", 3LL, 4LL, "rwx", 6, 7, 8, 9,
                       10ULL, 11ULL, 12, 13, 14, "times", "NOPERS");

        y = DKS_NEW("id=0 addr=helo fd=2 name=hello2 age=3 idle=4 flags=rwx "
                    "db=6 sub=7 psub=8 multi=9 qbuf=10 qbuf-free=11 obl=12 "
                    "oll=13 omem=14 events=times cmd=NOPERS");
        testCond("Verify multi-DKS_CATFMT", DKS_CMP(x, y), 0, "%d");

        DKS_FREE(y);
        DKS_FREE(x);
    }

    if (!bigTests) {
        printf("Stopping early because requested skip big allocation tests\n");
        testReport();
        return __failed_tests;
    }

    {
        printf("NOTE: The remaining tests allocate 4 GB to 10 GB RAM.\n");
        printf("Testing > 4 GB dks...\n");
        size_t len = ((size_t)1 << 32) + 1024;
        char *poo = zmalloc(len);
        memset(poo, 7, len);
        DKS_TYPE *x = DKS_NEWLEN(poo, len);
        zfree(poo);
        testCond("Verify create > 4 GB dks", DKS_LEN(x), len, "%zu");
        x = DKS_EXPANDBY(x, 1024);
        /* Note: this is the same size check as above because
         *       we just did EXPANDBY (added more buffer space),
         *       we didn't actually change the data length. */
        testCond("Verify grow > 4 GB dks", DKS_LEN(x), len, "%zu");

        DKS_FREE(x);
    }

    struct testLimit initialLimits[] = {
        /* GROW TESTS */
        {.startType = DKS_8,
         .maxContentSize = DKS_8_SHARED_MAX,
         .incrby = 20,
         .newType = DKS_8},
        {.startType = DKS_16,
         .maxContentSize = DKS_16_SHARED_MAX,
         .incrby = 20,
         .newType = DKS_8},
        {.startType = DKS_24,
         .maxContentSize = DKS_24_SHARED_MAX,
         .incrby = 20,
         .newType = DKS_24},
        {.startType = DKS_32,
         .maxContentSize = DKS_32_SHARED_MAX,
         .incrby = 20,
         .newType = DKS_32},
        {.startType = DKS_40,
         .maxContentSize = ((uint64_t)1 << 32) + 4096,
         .incrby = 20,
         .newType = DKS_40},
    };

    printf("Beginning creation tests...\n");
    for (size_t i = 0; i < sizeof(initialLimits) / sizeof(*initialLimits);
         i++) {
        struct testLimit limit = initialLimits[i];
        printf("[%s] Testing initial creation type for %s...\n", DKS_IMPL_STR,
               DKS_TYPENAME(limit.startType));

        size_t startSize = limit.maxContentSize; /* a size in this type */

        uint8_t *buf = zmalloc(startSize); /* WARNING: can be big (> 4 GB) */
        DKS_RANDBYTES(buf, startSize);

        DKS_TYPE *testing = DKS_NEWLEN(buf, startSize);
        printf("Initial type: %d\n", DKS_TYPE_GET(testing));

        testCond("dks created as expected type",
                 (uint32_t)DKS_TYPE_GET(testing), (uint32_t)limit.startType,
                 "%" PRIu32);
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");
        testCond("dks content matches source buffer",
                 memcmp(testing, buf, startSize), 0, "%d");

        DKS_CLEAR(testing);
        testCond("dks clear removes length", DKS_LEN(testing), (size_t)0,
                 "%zu");
        printf("After clear type: %d\n", DKS_TYPE_GET(testing));

#if !defined(DATAKIT_DKS_COMPACT)
        /* 'free' can't always hold total length, so adjust for MAX FREE */
        size_t maxfree = DKS_MAXSHAREDFORTYPE(DKS_TYPE_GET(testing));
        printf("Max free suggested as: %zu\n", maxfree);
        if (startSize > maxfree) {
            startSize = maxfree;
        }

        testCond("dks clear sets space free", DKS_AVAIL(testing), startSize,
                 "%zu");
#endif

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks remove free spaces keeps zero length", DKS_LEN(testing),
                 (size_t)0, "%zu");
        testCond("dks remove free spaces sets zero free", DKS_AVAIL(testing),
                 (size_t)0, "%zu");

        testing = DKS_GROWZERO(testing, limit.startType);
        testCond("dks growzero creates proper type", DKS_TYPE_GET(testing),
                 limit.startType, "%d");
        testCond("dks growzero sets correct length", DKS_LEN(testing),
                 (size_t)limit.startType, "%zu");

        DKS_FREE(testing);
        zfree(buf);
    }

#if !defined(DATAKIT_DKS_COMPACT)
    /* For now, don't run these in COMPACT mode because they are using different
     * size classes for start lengths (assuming full size availablilty and not
     * shared lengths for sizes+types like COMPACT has, so some of the math
     * assert assumptions don't apply for compact and we aren't refactoring
     * this batch of tests yet). */
    struct testLimit growLimits[] = {
        /* GROW TESTS */
        {.startType = DKS_8,
         .maxContentSize = DKS_8_FULL_MAX,
         .incrby = 20,
         .newType = DKS_16},
        {.startType = DKS_16,
         .maxContentSize = DKS_16_FULL_MAX,
         .incrby = 20,
         .newType = DKS_24},
        {.startType = DKS_24,
         .maxContentSize = DKS_24_FULL_MAX,
         .incrby = 20,
         .newType = DKS_32},
        {.startType = DKS_32,
         .maxContentSize = DKS_32_FULL_MAX,
         .incrby = 20,
         .newType = DKS_40},
#if 0
        {.startType = DKS_48,
         /* we can't test DKS_48 because most of our test boxes
          * can't allocate 1 TB of RAM (2^40 bytes) for testing. */
         .maxContentSize = (size_t)DKS_32_FULL_MAX + 8192,
         .incrby = 4096,
         .newType = DKS_48},
#endif
    };

    printf("Beginning conversion tests...\n");
    for (size_t i = 0; i < sizeof(growLimits) / sizeof(*growLimits); i++) {
        struct testLimit limit = growLimits[i];
        printf("Testing conversion from %s to %s...\n",
               DKS_TYPENAME(limit.startType), DKS_TYPENAME(limit.newType));

        size_t belowSize = limit.maxContentSize -
                           limit.incrby; /* e.g. MAX - 20 == below max */
        size_t exactSize =
            belowSize + limit.incrby; /* e.g. MAX - 20 + 20 == at max */

        size_t aboveSize =
            exactSize + limit.incrby; /* e.g. MAX - 20 + 20 + 20 == above max */
        (void)aboveSize;              /* not used for COMPACT testing */

        uint8_t *belowlimitbuf =
            zmalloc(belowSize); /* WARNING: can be big (> 4 GB) */
        DKS_RANDBYTES(belowlimitbuf, belowSize);

        DKS_TYPE *testing = DKS_NEWLEN(belowlimitbuf, belowSize);
        testCond("dks created properly below limit", DKS_TYPE_GET(testing),
                 limit.startType, "%u");
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");
        testCond("dks content matches source buffer",
                 memcmp(testing, belowlimitbuf, belowSize), 0, "%d");

        uint8_t *exactaddbuf = zmalloc(limit.incrby);
        DKS_RANDBYTES(exactaddbuf, limit.incrby);
        testing = DKS_EXPANDBYEXACT(testing, limit.incrby);
        testing = DKS_CATLEN(testing, exactaddbuf, limit.incrby);

        testCond("dks grew to exactly limit", DKS_LEN(testing),
                 belowSize + limit.incrby, "%zu");

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");

#if 0
        /* We don't currently have a 'DKS_COMPACTIFY_STORAGE_TYPE' function,
         * so even though we removed free space above, it can _still_ remain
         * a larger type due to the previously allocated space that used to
         * exist.
         * The size differences over expectations are because now we round up
         * requested allocations to the actual size that'll be allocated by
         * jemalloc instead of using allocator-unaware memory patterns. */
        testCond("dks type stayed same", DKS_TYPE_GET(testing), limit.startType,
                 "%u");
#endif

        testCond("dks initial contents still match source buffer",
                 memcmp(testing, belowlimitbuf, belowSize), 0, "%d");
        testCond("dks new contents match source buffer",
                 memcmp(testing + belowSize, exactaddbuf, limit.incrby), 0,
                 "%d");

        uint8_t *abovelimitbuf = zmalloc(limit.incrby);
        DKS_RANDBYTES(abovelimitbuf, limit.incrby);
        testing = DKS_EXPANDBYEXACT(testing, limit.incrby);
        testing = DKS_CATLEN(testing, abovelimitbuf, limit.incrby);

        testCond("dks grew above limit", DKS_LEN(testing),
                 belowSize + limit.incrby + limit.incrby, "%zu");

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");
        testCond("dks grew to new type", DKS_TYPE_GET(testing), limit.newType,
                 "%u");

        testCond("dks initial contents _still_ match source buffer",
                 memcmp(testing, belowlimitbuf, belowSize), 0, "%d");
        testCond("dks new contents match grow (1) source buffer",
                 memcmp(testing + belowSize, exactaddbuf, limit.incrby), 0,
                 "%d");
        testCond("dks new contents match grow (2) source buffer",
                 memcmp(testing + exactSize, abovelimitbuf, limit.incrby), 0,
                 "%d");

        DKS_CLEAR(testing);
        testCond("dks clear removes length", DKS_LEN(testing), (size_t)0,
                 "%zu");
#if !defined(DATAKIT_DKS_COMPACT)
        testCond("dks clear sets all space free", DKS_AVAIL(testing), aboveSize,
                 "%zu");
#endif

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks remove free spaces keeps zero length", DKS_LEN(testing),
                 (size_t)0, "%zu");
        testCond("dks remove free spaces sets zero free", DKS_AVAIL(testing),
                 (size_t)0, "%zu");

        testing = DKS_GROWZERO(testing, limit.startType);
        testCond("dks growzero creates proper type", DKS_TYPE_GET(testing),
                 limit.newType, "%d"); /* new because growby overallocates */
        testCond("dks growzero sets correct length", DKS_LEN(testing),
                 (size_t)limit.startType, "%zu");

        zfree(abovelimitbuf);
        zfree(exactaddbuf);
        DKS_FREE(testing);
        zfree(belowlimitbuf);
    }
#endif

    struct testLimit shrinkLimits[] = {
        /* VERIFY LARGE TYPES DON'T SHRINK */
        {.startType = DKS_40,
         .maxContentSize = (size_t)DKS_32_FULL_MAX + 8192,
         .incrby = -65536,
         .newType = DKS_40},
        {.startType = DKS_32,
         .maxContentSize = DKS_24_FULL_MAX * 2,
         .incrby = -(DKS_24_FULL_MAX + 128),
         .newType = DKS_32},
        {.startType = DKS_24,
         .maxContentSize = DKS_16_FULL_MAX * 2,
         .incrby = -(DKS_16_FULL_MAX + 128),
         .newType = DKS_24},
        {.startType = DKS_16,
         .maxContentSize = DKS_8_FULL_MAX * 2,
         .incrby = -(DKS_8_FULL_MAX + 128),
         .newType = DKS_16},
    };

    for (size_t i = 0; i < sizeof(shrinkLimits) / sizeof(*shrinkLimits); i++) {
        struct testLimit limit = shrinkLimits[i];
        printf("Testing non-conversion for %s...\n",
               DKS_TYPENAME(limit.startType));

        size_t startSize = limit.maxContentSize; /* MAX of smaller type + N */
        size_t shrinkSize =
            startSize + limit.incrby; /* smaller type max + -(N+K) */

        uint8_t *startbuf =
            zmalloc(startSize); /* WARNING: can be big (> 4 GB) */
        DKS_RANDBYTES(startbuf, startSize);

        DKS_TYPE *testing = DKS_NEWLEN(startbuf, startSize);
        testCond("dks created properly below limit",
                 (uint32_t)DKS_TYPE_GET(testing), (uint32_t)limit.startType,
                 "%" PRIu32);
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");
        testCond("dks contents match source buffer",
                 memcmp(testing, startbuf, startSize), 0, "%d");

        DKS_UPDATELENFORCE(testing, shrinkSize);
        testCond("dks shrunk to exactly limit", DKS_LEN(testing), shrinkSize,
                 "%zu");
#if !defined(DATAKIT_DKS_COMPACT)
        testCond("dks has correct avail", DKS_AVAIL(testing),
                 (startSize - shrinkSize), "%zu");
#endif
        testCond("dks type remains same", (uint32_t)DKS_TYPE_GET(testing),
                 (uint32_t)limit.startType, "%" PRIu32);
        testCond("dks initial contents still match source buffer",
                 memcmp(testing, startbuf, shrinkSize), 0, "%d");

        DKS_CLEAR(testing);
        testCond("dks clear removes length", DKS_LEN(testing), (size_t)0,
                 "%zu");
#if !defined(DATAKIT_DKS_COMPACT)
        testCond("dks now has free space of original allocation",
                 DKS_AVAIL(testing), startSize, "%zu");
#endif

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks remove free spaces keeps zero length", DKS_LEN(testing),
                 (size_t)0, "%zu");
        testCond("dks remove free spaces sets zero free", DKS_AVAIL(testing),
                 (size_t)0, "%zu");

        testing = DKS_GROWZERO(testing, limit.startType);
        testCond("dks growzero creates proper type", DKS_TYPE_GET(testing),
                 limit.startType, "%d");
        testCond("dks growzero sets correct length", DKS_LEN(testing),
                 (size_t)limit.startType, "%zu");

        DKS_FREE(testing);
        zfree(startbuf);
    }

    struct testLimit growbyLimits[] = {
        /* VERIFY GROWTH HAPPENS WHEN FREE > SHARED_MAX */
        {.startType = DKS_8,
         .maxContentSize = DKS_8_SHARED_MAX - 32,
         .incrby = DKS_8_SHARED_MAX + 64,
         .newType = DKS_16},
        {.startType = DKS_16,
         .maxContentSize = DKS_16_SHARED_MAX - 128,
         .incrby = DKS_16_SHARED_MAX + 64,
         .newType = DKS_24},
        {.startType = DKS_24,
         .maxContentSize = DKS_24_SHARED_MAX - 128,
         .incrby = DKS_24_SHARED_MAX + 64,
         .newType = DKS_32},
        {.startType = DKS_32,
         .maxContentSize = DKS_32_SHARED_MAX - 1024,
         .incrby = DKS_32_SHARED_MAX + 64,
         .newType = DKS_40},
    };

    for (size_t i = 0; i < sizeof(growbyLimits) / sizeof(*growbyLimits); i++) {
        struct testLimit limit = growbyLimits[i];
        printf("Testing grow happens at proper limits %s...\n",
               DKS_TYPENAME(limit.startType));

        size_t startSize =
            limit.maxContentSize; /* Slightly below maximum free limit. */
        size_t growby =
            limit.incrby; /* amount will grow us larger than free limit. */

        uint8_t *startbuf =
            zmalloc(startSize); /* WARNING: can be big (~1 GB) */
        DKS_RANDBYTES(startbuf, startSize);

        DKS_TYPE *testing = DKS_NEWLEN(startbuf, startSize);
        testCond("dks created properly below limit",
                 (uint32_t)DKS_TYPE_GET(testing), (uint32_t)limit.startType,
                 "%" PRIu32);
        testCond("dks has no avail", DKS_AVAIL(testing), (size_t)0, "%zu");
        testCond("dks contents match source buffer",
                 memcmp(testing, startbuf, startSize), 0, "%d");

        testing = DKS_EXPANDBYEXACT(testing, growby);
        /* We can't do exact avail checking anymore because we automatically
         * round requests up to the next allocator bucket size. */
#if !defined(DATAKIT_DKS_COMPACT)
        testCondGte("dks has correct avail", DKS_AVAIL(testing), growby, "%zu");
#endif
        testCond("dks type upgraded itself", (uint32_t)DKS_TYPE_GET(testing),
                 (uint32_t)limit.newType, "%" PRIu32);
        testCond("dks initial contents still match source buffer",
                 memcmp(testing, startbuf, startSize), 0, "%d");

        DKS_CLEAR(testing);
        testCond("dks clear removes length", DKS_LEN(testing), (size_t)0,
                 "%zu");
#if !defined(DATAKIT_DKS_COMPACT)
        testCondGte("dks now has only free space", DKS_AVAIL(testing),
                    startSize + growby, "%zu");
#endif

        testing = DKS_REMOVEFREESPACE(testing);
        testCond("dks remove free spaces keeps zero length", DKS_LEN(testing),
                 (size_t)0, "%zu");
        testCond("dks remove free spaces sets zero free", DKS_AVAIL(testing),
                 (size_t)0, "%zu");

        testing = DKS_GROWZERO(testing, limit.startType);
        testCond("dks growzero creates proper type", DKS_TYPE_GET(testing),
                 limit.newType, "%d"); /* new because growby overallocates */
        testCond("dks growzero sets correct length", DKS_LEN(testing),
                 (size_t)limit.startType, "%zu");

        /* implicit test for DKS_NATIVE() */
        zfree(DKS_NATIVE(testing, NULL));
        zfree(startbuf);
    }

    testReport();
    return __failed_tests;
}

int DKS_BENCHMAIN(void) {
    for (int j = 0; j < 1000000; j++) {
        DKS_TYPE *s = DKS_EMPTYLEN(0);
        for (int i = 0; i < 200; i++) {
            s = DKS_CATLEN(s, "abc", 3);
        }

        DKS_FREE(s);
    }

    return -3;
}
#endif

/* original weaksauce sds implementation by:
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
