#include "varintExternal.h"
#include "endianIsLittle.h"

/* _varintExternalCopyToEncodingLittleEndian is an unrolled version of:
 *       for (int start = 0; start < encoding; start++) {
 *           dst[start] = src[start];
 *       }
 *
 * The unrolled loop version is about 20% faster than a direct loop. */
/* Little endian is simple because our storage format is little endian as well.
 * We can copy to/from storage and system just by copying bytes in order. */
static void
varintExternalCopyToEncodingLittleEndian_(uint8_t *restrict dst,
                                          const uint8_t *restrict src,
                                          const varintWidth encoding) {
    switch (encoding) {
    case VARINT_WIDTH_8B:
        dst[0] = src[0];
        break;

    case VARINT_WIDTH_16B:
        dst[1] = src[1];
        dst[0] = src[0];
        break;

    case VARINT_WIDTH_32B:
        dst[3] = src[3];
        varint_fallthrough;
    case VARINT_WIDTH_24B:
        dst[2] = src[2];
        dst[1] = src[1];
        dst[0] = src[0];
        break;

    case VARINT_WIDTH_56B:
        dst[6] = src[6];
        varint_fallthrough;
    case VARINT_WIDTH_48B:
        dst[5] = src[5];
        varint_fallthrough;
    case VARINT_WIDTH_40B:
        dst[4] = src[4];
        dst[3] = src[3];
        dst[2] = src[2];
        dst[1] = src[1];
        dst[0] = src[0];
        break;

    case VARINT_WIDTH_64B:
        memcpy(dst, src, sizeof(uint64_t));
        break;

    case VARINT_WIDTH_120B:
        dst[14] = src[14];
        varint_fallthrough;
    case VARINT_WIDTH_112B:
        dst[13] = src[13];
        varint_fallthrough;
    case VARINT_WIDTH_104B:
        dst[12] = src[12];
        varint_fallthrough;
    case VARINT_WIDTH_96B:
        dst[11] = src[11];
        varint_fallthrough;
    case VARINT_WIDTH_88B:
        dst[10] = src[10];
        varint_fallthrough;
    case VARINT_WIDTH_80B:
        dst[9] = src[9];
        varint_fallthrough;
    case VARINT_WIDTH_72B:
        dst[8] = src[8];
        memcpy(dst, src, 8);
        break;

    case VARINT_WIDTH_128B:
        memcpy(dst, src, sizeof(__uint128_t));
        break;

    default:
        assert(NULL && "Bad input width?");
        __builtin_unreachable();
    }
}

/* automatically determine 'encoding' then copy relevant bytes into 'dst' */
static varintWidth
varintExternalCopyUsedBytesLittleEndian_(uint8_t *dst, const uint64_t _src) {
    const uint8_t *src = (const uint8_t *)&_src;

    varintWidth encoding;
    varintExternalUnsignedEncoding(_src, encoding);
    varintExternalCopyToEncodingLittleEndian_(dst, src, encoding);
    return encoding;
}

static void varintExternalCopyToEncodingBigEndian_(uint8_t *restrict dst,
                                                   const uint8_t *src,
                                                   varintWidth encoding) {
    /* lazy big endian solution of just an opposite loop. */
    /* Example: if _src is 1 in big endian, that's 0x0000000000000001,
     * so you need to extract the src[encoding - 1] byte as the first
     * src in 'dst' (which is always little endian). */
    /* 'encoding' is the length of '_src' in bytes, so 1 through 8. */
    /* so, for encoding of 8 bytes, this does:
     *  dst[0] = src[7]; dst[1] = src[6]; ...; dst[7] = src[0] */
    uint8_t resPos = 0;
    uint8_t srcPos = encoding;

    while (srcPos > 0) {
        dst[resPos++] = src[--srcPos];
    }
}

static varintWidth varintExternalCopyUsedBytesBigEndian_(uint8_t *dst,
                                                         const uint64_t _src) {
    const uint8_t *src = (const uint8_t *)&_src;

    varintWidth encoding;
    varintExternalUnsignedEncoding(_src, encoding);
    varintExternalCopyToEncodingBigEndian_(dst, src, encoding);
    return encoding;
}

/* Read integer or float pointed to by 'src', store result in databox 'r' */
static uint64_t
varintExternalLoadFromEncodingLittleEndian_(const uint8_t *src,
                                            const varintWidth encoding) {
    uint64_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* little endian only has one encode/decode function since bytes are
     * stored in the same order for both data and machine representation. */
    varintExternalCopyToEncodingLittleEndian_(resarr, src, encoding);
    return result;
}

static uint64_t
varintExternalLoadFromEncodingBigEndian_(const uint8_t *restrict src,
                                         const varintWidth encoding) {
    uint64_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* Opposite of _varintExternalCopyToEncodingBigEndian.
     * Instead of doing dst[0] = src[7],
     * here we are doing result[7] = src[0] but
     * we stop when we run out of encoded bytes
     * (number of bytes == encoding value) */
    uint8_t resPos = 0;
    uint8_t srcPos = encoding;

    while (srcPos > 0) {
        resarr[resPos++] = src[--srcPos];
    }

    return result;
}

static __uint128_t
varintBigExternalLoadFromEncodingLittleEndian_(const uint8_t *src,
                                               const varintWidth encoding) {
    __uint128_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* little endian only has one encode/decode function since bytes are
     * stored in the same order for both data and machine representation. */
    varintExternalCopyToEncodingLittleEndian_(resarr, src, encoding);
    return result;
}

static __uint128_t
varintBigExternalLoadFromEncodingBigEndian_(const uint8_t *restrict src,
                                            const varintWidth encoding) {
    __uint128_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* Opposite of _varintExternalCopyToEncodingBigEndian.
     * Instead of doing dst[0] = src[7],
     * here we are doing result[7] = src[0] but
     * we stop when we run out of encoded bytes
     * (number of bytes == encoding value) */
    uint8_t resPos = 0;
    uint8_t srcPos = encoding;

    while (srcPos > 0) {
        resarr[resPos++] = src[--srcPos];
    }

    return result;
}

varintWidth varintExternalSignedEncoding(int64_t value) {
    if (value < 0 || value > INT64_MAX) {
        assert(NULL && "Invalid signed storage attempt!");
        __builtin_unreachable();
        /* you should manually convert/cast your number to "unsigned" (with
         * overflow), get storage size, store as unsigned (but record it with
         * a signed box), then on retrieval, re-cast to signed storage. */
    }

    varintWidth encoding;
    varintExternalUnsignedEncoding(value, encoding);
    return encoding;
}

varintWidth varintExternalPut(void *p, uint64_t v) {
    if (endianIsLittle()) {
        return varintExternalCopyUsedBytesLittleEndian_(p, v);
    }

    return varintExternalCopyUsedBytesBigEndian_(p, v);
}

/* Like above, but always use exactly 'encoding' bytes.  This can help with
 * varint math where you don't want to shrink an allocation if a number
 * becomes small. */
void varintExternalPutFixedWidth(void *p, uint64_t v, varintWidth encoding) {
    const uint8_t *src = (const uint8_t *)&v;
    if (endianIsLittle()) {
        varintExternalCopyToEncodingLittleEndian_(p, src, encoding);
    } else {
        varintExternalCopyToEncodingBigEndian_(p, src, encoding);
    }
}

void varintExternalPutFixedWidthBig(void *p, __uint128_t v,
                                    varintWidth encoding) {
    const uint8_t *src = (const uint8_t *)&v;
    if (endianIsLittle()) {
        varintExternalCopyToEncodingLittleEndian_(p, src, encoding);
    } else {
        varintExternalCopyToEncodingBigEndian_(p, src, encoding);
    }
}

uint64_t varintExternalGet(const void *p, varintWidth encoding) {
    if (endianIsLittle()) {
        return varintExternalLoadFromEncodingLittleEndian_(p, encoding);
    }

    return varintExternalLoadFromEncodingBigEndian_(p, encoding);
}

__uint128_t varintBigExternalGet(const void *p, varintWidth encoding) {
    if (endianIsLittle()) {
        return varintBigExternalLoadFromEncodingLittleEndian_(p, encoding);
    }

    return varintBigExternalLoadFromEncodingBigEndian_(p, encoding);
}

static varintWidth varintExternalAdd_(uint8_t *p, varintWidth origEncoding,
                                      int64_t add, bool force) {
    /* we're pulling the value out as just an 'int64_t' because we want to
     * allow signed math. */
    uint64_t retrieve = varintExternalGet(p, origEncoding);
    int64_t updatingVal = (int64_t)retrieve;
    long long newVal; /* long long because it's the result of a builtin */

    VARINT_ADD_OR_ABORT_OVERFLOW_(updatingVal, add, newVal);

    varintWidth newEncoding;
    varintExternalUnsignedEncoding(updatingVal, newEncoding);

    /* If new encoding is larger than current encoding, we don't
     * want to overwrite memory beyond our current varint.
     * Bail out unless this was requested as "safe to grow" addition. */
    if (newEncoding > origEncoding && !force) {
        return newEncoding;
    }

    varintExternalPut(p, newVal);
    return newEncoding;
}

/* If math can't fit into current encoding, fail the write and return
 * the new encoding length we need for this math to complete.
 * (Then the user can manually run the add to update.) */
varintWidth varintExternalAddNoGrow(uint8_t *p, varintWidth encoding,
                                    int64_t add) {
    return varintExternalAdd_(p, encoding, add, false);
}

varintWidth varintExternalAddGrow(uint8_t *p, varintWidth encoding,
                                  int64_t add) {
    return varintExternalAdd_(p, encoding, add, true);
}
