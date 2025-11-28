#pragma once

#include "varint.h"
#include "varintExternal.h"

__BEGIN_DECLS

/* ====================================================================
 * Group varints
 * ==================================================================== */
/* varint model Group Container:
 *   Type encoded inside: field count + width bitmap
 *   Size: 2+ bytes (field_count + bitmap + values)
 *   Layout: [field_count][widths_bitmap][value1][value2]...[valueN]
 *   Meaning: Encode multiple related values with shared metadata
 *   Pro: Reduces per-field overhead, fast random field access
 *   Con: All fields must be known at encode time */

/* Maximum fields per group (limited by practical bitmap size) */
#define VARINT_GROUP_MAX_FIELDS 64

/* Width encoding uses 2 bits per field:
 *   00 = 1 byte  (VARINT_WIDTH_8B)
 *   01 = 2 bytes (VARINT_WIDTH_16B)
 *   10 = 4 bytes (VARINT_WIDTH_32B)
 *   11 = 8 bytes (VARINT_WIDTH_64B)
 */
#define VARINT_GROUP_WIDTH_BITS 2
#define VARINT_GROUP_WIDTH_MASK 0x03

/* Map 2-bit encoding to actual byte widths */
static inline varintWidth varintGroupWidthDecode_(uint8_t encoded) {
    switch (encoded & VARINT_GROUP_WIDTH_MASK) {
    case 0:
        return VARINT_WIDTH_8B;
    case 1:
        return VARINT_WIDTH_16B;
    case 2:
        return VARINT_WIDTH_32B;
    case 3:
        return VARINT_WIDTH_64B;
    default:
        return VARINT_WIDTH_INVALID;
    }
}

/* Map actual byte width to 2-bit encoding */
static inline uint8_t varintGroupWidthEncode_(varintWidth width) {
    switch (width) {
    case VARINT_WIDTH_8B:
        return 0;
    case VARINT_WIDTH_16B:
        return 1;
    case VARINT_WIDTH_24B:
        return 2; /* upgrade to 4 bytes */
    case VARINT_WIDTH_32B:
        return 2;
    case VARINT_WIDTH_40B:
        return 3; /* upgrade to 8 bytes */
    case VARINT_WIDTH_48B:
        return 3;
    case VARINT_WIDTH_56B:
        return 3;
    case VARINT_WIDTH_64B:
        return 3;
    default:
        return 3; /* default to 8 bytes for safety */
    }
}

/* Encode group of values into buffer
 * Returns: total bytes written, or 0 on error */
size_t varintGroupEncode(uint8_t *dst, const uint64_t *values,
                         uint8_t fieldCount);

/* Decode group of values from buffer
 * Returns: total bytes read, or 0 on error */
size_t varintGroupDecode(const uint8_t *src, uint64_t *values,
                         uint8_t *fieldCount, size_t maxFields);

/* Convenience wrapper for single group encode
 * Same as varintGroupEncode but more explicit naming */
static inline size_t varintGroupPut(uint8_t *dst, const uint64_t *values,
                                    uint8_t fieldCount) {
    return varintGroupEncode(dst, values, fieldCount);
}

/* Convenience wrapper for single group decode
 * Same as varintGroupDecode but more explicit naming */
static inline size_t varintGroupGet(const uint8_t *src, uint64_t *values,
                                    uint8_t *fieldCount, size_t maxFields) {
    return varintGroupDecode(src, values, fieldCount, maxFields);
}

/* Extract specific field from encoded group without full decode
 * Returns: bytes consumed from buffer, or 0 on error
 * Sets *value to the extracted field value */
size_t varintGroupGetField(const uint8_t *src, uint8_t fieldIndex,
                           uint64_t *value);

/* Calculate encoded size for a group of values
 * Returns: total bytes needed, or 0 on error */
size_t varintGroupSize(const uint64_t *values, uint8_t fieldCount);

/* Calculate encoded size from already-encoded group
 * Returns: total bytes used by group, or 0 on error */
size_t varintGroupGetSize(const uint8_t *src);

/* Get field count from encoded group */
static inline uint8_t varintGroupGetFieldCount(const uint8_t *src) {
    return src[0];
}

/* Get width of specific field from encoded group */
varintWidth varintGroupGetFieldWidth(const uint8_t *src, uint8_t fieldIndex);

/* Calculate bitmap size needed for fieldCount fields */
static inline size_t varintGroupBitmapSize_(uint8_t fieldCount) {
    /* 2 bits per field, round up to whole bytes */
    return (fieldCount * VARINT_GROUP_WIDTH_BITS + 7) / 8;
}

__END_DECLS
