#include "varintGroup.h"

/*
** This file contains routines for encoding groups of related values
** with shared metadata (field count and width bitmap).
**
** A group varint encoding consists of:
**   1. Field count (1 byte)
**   2. Width bitmap (variable size, 2 bits per field)
**   3. Values (each encoded in their specified width)
**
** Width encoding uses 2 bits per field:
**   00 = 1 byte
**   01 = 2 bytes
**   10 = 4 bytes
**   11 = 8 bytes
**
** This is ideal for encoding struct-like data where you have a small
** number of related fields that often fit in smaller widths.
**
** Example: [age=25, salary=50000, zip=94102, time=1700000000]
**   Field count: 4
**   Widths: [1, 3, 3, 4] -> [1, 4, 4, 4] (normalized) -> 00 10 10 10
**   Bitmap: 0b00101010 = 0x2A (1 byte)
**   Encoded: [4][0x2A][25][50000][94102][1700000000]
**   Total: 1 + 1 + 1 + 4 + 4 + 4 = 15 bytes
*/

/* Encode group of values into buffer */
size_t varintGroupEncode(uint8_t *dst, const uint64_t *values,
                         uint8_t fieldCount) {
    if (fieldCount == 0 || fieldCount > VARINT_GROUP_MAX_FIELDS) {
        return 0;
    }

    /* Store field count */
    dst[0] = fieldCount;
    size_t offset = 1;

    /* Calculate bitmap size */
    size_t bitmapSize = varintGroupBitmapSize_(fieldCount);

    /* Clear bitmap area */
    memset(dst + offset, 0, bitmapSize);

    /* Determine width for each field and build bitmap */
    varintWidth widths[VARINT_GROUP_MAX_FIELDS];
    for (uint8_t i = 0; i < fieldCount; i++) {
        varintWidth actualWidth;
        varintExternalUnsignedEncoding(values[i], actualWidth);

        /* Normalize to supported widths (1, 2, 4, 8) */
        varintWidth normalizedWidth;
        if (actualWidth <= VARINT_WIDTH_8B) {
            normalizedWidth = VARINT_WIDTH_8B;
        } else if (actualWidth <= VARINT_WIDTH_16B) {
            normalizedWidth = VARINT_WIDTH_16B;
        } else if (actualWidth <= VARINT_WIDTH_32B) {
            normalizedWidth = VARINT_WIDTH_32B;
        } else {
            normalizedWidth = VARINT_WIDTH_64B;
        }

        widths[i] = normalizedWidth;

        /* Encode width into bitmap (2 bits per field) */
        uint8_t encoded = varintGroupWidthEncode_(normalizedWidth);
        size_t bitPos = i * VARINT_GROUP_WIDTH_BITS;
        size_t bytePos = offset + (bitPos / 8);
        size_t bitOffset = bitPos % 8;

        dst[bytePos] |= (encoded << bitOffset);
    }

    offset += bitmapSize;

    /* Encode all values */
    for (uint8_t i = 0; i < fieldCount; i++) {
        varintExternalPutFixedWidth(dst + offset, values[i], widths[i]);
        offset += widths[i];
    }

    return offset;
}

/* Decode group of values from buffer */
size_t varintGroupDecode(const uint8_t *src, uint64_t *values,
                         uint8_t *fieldCount, size_t maxFields) {
    /* Read field count */
    uint8_t count = src[0];
    if (count == 0 || count > VARINT_GROUP_MAX_FIELDS || count > maxFields) {
        return 0;
    }

    *fieldCount = count;
    size_t offset = 1;

    /* Calculate bitmap size */
    size_t bitmapSize = varintGroupBitmapSize_(count);

    /* Decode widths from bitmap */
    varintWidth widths[VARINT_GROUP_MAX_FIELDS];
    for (uint8_t i = 0; i < count; i++) {
        size_t bitPos = i * VARINT_GROUP_WIDTH_BITS;
        size_t bytePos = offset + (bitPos / 8);
        size_t bitOffset = bitPos % 8;

        uint8_t encoded = (src[bytePos] >> bitOffset) & VARINT_GROUP_WIDTH_MASK;
        widths[i] = varintGroupWidthDecode_(encoded);
    }

    offset += bitmapSize;

    /* Decode all values */
    for (uint8_t i = 0; i < count; i++) {
        values[i] = varintExternalGet(src + offset, widths[i]);
        offset += widths[i];
    }

    return offset;
}

/* Extract specific field from encoded group without full decode */
size_t varintGroupGetField(const uint8_t *src, uint8_t fieldIndex,
                           uint64_t *value) {
    /* Read field count */
    uint8_t count = src[0];
    if (count == 0 || fieldIndex >= count) {
        return 0;
    }

    size_t offset = 1;
    size_t bitmapSize = varintGroupBitmapSize_(count);

    /* Decode width for requested field */
    size_t bitPos = fieldIndex * VARINT_GROUP_WIDTH_BITS;
    size_t bytePos = offset + (bitPos / 8);
    size_t bitOffset = bitPos % 8;

    uint8_t encoded = (src[bytePos] >> bitOffset) & VARINT_GROUP_WIDTH_MASK;
    varintWidth targetWidth = varintGroupWidthDecode_(encoded);

    offset += bitmapSize;

    /* Skip earlier fields */
    for (uint8_t i = 0; i < fieldIndex; i++) {
        bitPos = i * VARINT_GROUP_WIDTH_BITS;
        bytePos = 1 + (bitPos / 8);
        bitOffset = bitPos % 8;

        encoded = (src[bytePos] >> bitOffset) & VARINT_GROUP_WIDTH_MASK;
        varintWidth width = varintGroupWidthDecode_(encoded);
        offset += width;
    }

    /* Extract target field */
    *value = varintExternalGet(src + offset, targetWidth);

    /* Return total bytes consumed from start of buffer */
    offset += targetWidth;
    return offset;
}

/* Calculate encoded size for a group of values */
size_t varintGroupSize(const uint64_t *values, uint8_t fieldCount) {
    if (fieldCount == 0 || fieldCount > VARINT_GROUP_MAX_FIELDS) {
        return 0;
    }

    size_t totalSize = 1; /* field count */
    totalSize += varintGroupBitmapSize_(fieldCount);

    /* Calculate total value size */
    for (uint8_t i = 0; i < fieldCount; i++) {
        varintWidth actualWidth;
        varintExternalUnsignedEncoding(values[i], actualWidth);

        /* Normalize to supported widths */
        if (actualWidth <= VARINT_WIDTH_8B) {
            totalSize += VARINT_WIDTH_8B;
        } else if (actualWidth <= VARINT_WIDTH_16B) {
            totalSize += VARINT_WIDTH_16B;
        } else if (actualWidth <= VARINT_WIDTH_32B) {
            totalSize += VARINT_WIDTH_32B;
        } else {
            totalSize += VARINT_WIDTH_64B;
        }
    }

    return totalSize;
}

/* Calculate encoded size from already-encoded group */
size_t varintGroupGetSize(const uint8_t *src) {
    uint8_t count = src[0];
    if (count == 0 || count > VARINT_GROUP_MAX_FIELDS) {
        return 0;
    }

    size_t offset = 1;
    size_t bitmapSize = varintGroupBitmapSize_(count);
    offset += bitmapSize;

    /* Sum up all field widths */
    for (uint8_t i = 0; i < count; i++) {
        size_t bitPos = i * VARINT_GROUP_WIDTH_BITS;
        size_t bytePos = 1 + (bitPos / 8);
        size_t bitOffset = bitPos % 8;

        uint8_t encoded = (src[bytePos] >> bitOffset) & VARINT_GROUP_WIDTH_MASK;
        varintWidth width = varintGroupWidthDecode_(encoded);
        offset += width;
    }

    return offset;
}

/* Get width of specific field from encoded group */
varintWidth varintGroupGetFieldWidth(const uint8_t *src, uint8_t fieldIndex) {
    uint8_t count = src[0];
    if (count == 0 || fieldIndex >= count) {
        return VARINT_WIDTH_INVALID;
    }

    size_t bitPos = fieldIndex * VARINT_GROUP_WIDTH_BITS;
    size_t bytePos = 1 + (bitPos / 8);
    size_t bitOffset = bitPos % 8;

    uint8_t encoded = (src[bytePos] >> bitOffset) & VARINT_GROUP_WIDTH_MASK;
    return varintGroupWidthDecode_(encoded);
}
