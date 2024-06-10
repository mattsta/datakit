#include "varintExternalBigEndian.h"
#include "endianIsLittle.h"

/* The unrolled loop version is about 20% faster than a direct loop. */
static void _varintExternalBigEndianCopyToEncodingLittleEndian(
    uint8_t *__restrict dst, const uint8_t *__restrict src,
    const varintWidth encoding) {
#if 0
    uint8_t resPos = 0;
    uint8_t srcPos = encoding;
    while (srcPos > 0) {
        dst[resPos++] = src[--srcPos];
    }
#else
    switch (encoding) {
    case VARINT_WIDTH_8B:
        dst[0] = src[0];
        break;

    case VARINT_WIDTH_16B:
        dst[1] = src[0];
        dst[0] = src[1];
        break;

    case VARINT_WIDTH_32B:
        dst[3] = src[0];
        dst[2] = src[1];
        dst[1] = src[2];
        dst[0] = src[3];
        break;

    case VARINT_WIDTH_24B:
        dst[2] = src[0];
        dst[1] = src[1];
        dst[0] = src[2];
        break;

    case VARINT_WIDTH_56B:
        dst[6] = src[0];
        dst[5] = src[1];
        dst[4] = src[2];
        dst[3] = src[3];
        dst[2] = src[4];
        dst[1] = src[5];
        dst[0] = src[6];
        break;

    case VARINT_WIDTH_48B:
        dst[5] = src[0];
        dst[4] = src[1];
        dst[3] = src[2];
        dst[2] = src[3];
        dst[1] = src[4];
        dst[0] = src[5];
        break;

    case VARINT_WIDTH_40B:
        dst[4] = src[0];
        dst[3] = src[1];
        dst[2] = src[2];
        dst[1] = src[3];
        dst[0] = src[4];
        break;

    case VARINT_WIDTH_64B:
        *(uint64_t *)dst = __builtin_bswap64(*(uint64_t *)src);
        break;

    default:
        assert(NULL);
    }
#endif
}

/* automatically determine 'encoding' then copy relevant bytes into 'dst' */
static varintWidth
_varintExternalBigEndianCopyUsedBytesLittleEndian(uint8_t *dst,
                                                  const uint64_t _src) {
    const uint8_t *src = (const uint8_t *)&_src;

    varintWidth encoding;
    varintExternalBigEndianUnsignedEncoding(_src, encoding);
    _varintExternalBigEndianCopyToEncodingLittleEndian(dst, src, encoding);
    return encoding;
}

static void _varintExternalBigEndianCopyToEncodingBigEndian(
    uint8_t *__restrict dst, const uint8_t *src, varintWidth encoding) {
    for (varintWidth start = 0; start < encoding; start++) {
        dst[start] = src[start];
    }
}

static varintWidth
_varintExternalBigEndianCopyUsedBytesBigEndian(uint8_t *dst,
                                               const uint64_t _src) {
    const uint8_t *src = (const uint8_t *)&_src;

    varintWidth encoding;
    varintExternalBigEndianUnsignedEncoding(_src, encoding);
    _varintExternalBigEndianCopyToEncodingBigEndian(dst, src, encoding);
    return encoding;
}

/* Read integer or float pointed to by 'src', store result in databox 'r' */
static uint64_t _varintExternalBigEndianLoadFromEncodingLittleEndian(
    const uint8_t *src, const varintWidth encoding) {
    uint64_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* little endian only has one encode/decode function since bytes are
     * stored in the same order for both data and machine representation. */
    _varintExternalBigEndianCopyToEncodingLittleEndian(resarr, src, encoding);
    return result;
}

static uint64_t
_varintExternalBigEndianLoadFromEncodingBigEndian(const uint8_t *__restrict src,
                                                  const varintWidth encoding) {
    uint64_t result = 0;
    uint8_t *resarr = (uint8_t *)&result;

    /* Opposite of _varintExternalBigEndianCopyToEncodingBigEndian.
     * Instead of doing dst[0] = src[7],
     * here we are doing result[7] = src[0] but
     * we stop when we run out of encoded bytes
     * (number of bytes == encoding value) */

    for (varintWidth start = 0; start < encoding; start++) {
        resarr[start] = src[start];
    }

    return result;
}

varintWidth varintExternalBigEndianPut(void *p, uint64_t v) {
    if (endianIsLittle()) {
        return _varintExternalBigEndianCopyUsedBytesLittleEndian(p, v);
    } else {
        return _varintExternalBigEndianCopyUsedBytesBigEndian(p, v);
    }
}

/* Like above, but always use exactly 'encoding' bytes.  This can help with
 * varint math where you don't want to shrink an allocation if a number
 * becomes small. */
void varintExternalBigEndianPutFixedWidth(void *p, uint64_t v,
                                          varintWidth encoding) {
    const uint8_t *src = (const uint8_t *)&v;
    if (endianIsLittle()) {
        _varintExternalBigEndianCopyToEncodingLittleEndian(p, src, encoding);
    } else {
        _varintExternalBigEndianCopyToEncodingBigEndian(p, src, encoding);
    }
}

uint64_t varintExternalBigEndianGet(const void *p, varintWidth encoding) {
    if (endianIsLittle()) {
        return _varintExternalBigEndianLoadFromEncodingLittleEndian(p,
                                                                    encoding);
    } else {
        return _varintExternalBigEndianLoadFromEncodingBigEndian(p, encoding);
    }
}
