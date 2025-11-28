#include "varintDelta.h"
#include <string.h>

/* Encode a single delta value into buffer
 * Format: [width_byte][delta_bytes...]
 * The width byte indicates how many bytes the delta value uses */
varintWidth varintDeltaPut(uint8_t *p, const int64_t delta) {
    /* Convert signed delta to unsigned via ZigZag */
    uint64_t zigzag = varintDeltaZigZag(delta);

    /* Determine width needed for this unsigned value */
    varintWidth width;
    varintExternalUnsignedEncoding(zigzag, width);

    /* Store width byte */
    p[0] = (uint8_t)width;

    /* Store delta value using varintExternal encoding */
    varintExternalPutFixedWidth(p + 1, zigzag, width);

    /* Return total bytes written: 1 (width) + width (data) */
    return 1 + width;
}

/* Decode a single delta value from buffer */
varintWidth varintDeltaGet(const uint8_t *p, int64_t *pDelta) {
    /* Read width byte */
    varintWidth width = (varintWidth)p[0];

    /* Read delta value */
    uint64_t zigzag = varintExternalGet(p + 1, width);

    /* Decode ZigZag to signed value */
    *pDelta = varintDeltaZigZagDecode(zigzag);

    /* Return total bytes read: 1 (width) + width (data) */
    return 1 + width;
}

/* Encode array of absolute values as base + deltas */
size_t varintDeltaEncode(uint8_t *output, const int64_t *values, size_t count) {
    if (count == 0) {
        return 0;
    }

    uint8_t *p = output;

    /* Store base value (first element) */
    int64_t base = values[0];
    varintWidth baseWidth;

    /* For base value, we need to handle signed values
     * Use unsigned encoding of absolute value, then store sign separately
     * Actually, simpler: just store as signed via zigzag */
    uint64_t baseZigZag = varintDeltaZigZag(base);
    varintExternalUnsignedEncoding(baseZigZag, baseWidth);

    /* Write base width and value */
    *p++ = (uint8_t)baseWidth;
    varintExternalPutFixedWidth(p, baseZigZag, baseWidth);
    p += baseWidth;

    /* Encode deltas */
    int64_t prev = base;
    for (size_t i = 1; i < count; i++) {
        int64_t delta = values[i] - prev;
        varintWidth deltaBytes = varintDeltaPut(p, delta);
        p += deltaBytes;
        prev = values[i];
    }

    return (size_t)(p - output);
}

/* Decode delta-encoded array back to absolute values */
size_t varintDeltaDecode(const uint8_t *input, size_t count, int64_t *output) {
    if (count == 0) {
        return 0;
    }

    const uint8_t *p = input;

    /* Read base value */
    varintWidth baseWidth = (varintWidth)(*p++);
    uint64_t baseZigZag = varintExternalGet(p, baseWidth);
    int64_t base = varintDeltaZigZagDecode(baseZigZag);
    p += baseWidth;

    output[0] = base;

    /* Decode deltas and reconstruct absolute values */
    int64_t current = base;
    for (size_t i = 1; i < count; i++) {
        int64_t delta;
        varintWidth deltaBytes = varintDeltaGet(p, &delta);
        p += deltaBytes;

        current += delta;
        output[i] = current;
    }

    return (size_t)(p - input);
}

/* Encode array of unsigned absolute values as base + deltas */
size_t varintDeltaEncodeUnsigned(uint8_t *output, const uint64_t *values,
                                 size_t count) {
    if (count == 0) {
        return 0;
    }

    uint8_t *p = output;

    /* Store base value (first element) - unsigned, no zigzag needed */
    uint64_t base = values[0];
    varintWidth baseWidth;
    varintExternalUnsignedEncoding(base, baseWidth);

    /* Write base width and value */
    *p++ = (uint8_t)baseWidth;
    varintExternalPutFixedWidth(p, base, baseWidth);
    p += baseWidth;

    /* Encode deltas (still need zigzag since deltas can be negative) */
    uint64_t prev = base;
    for (size_t i = 1; i < count; i++) {
        /* Calculate signed delta */
        int64_t delta = (int64_t)(values[i] - prev);
        varintWidth deltaBytes = varintDeltaPut(p, delta);
        p += deltaBytes;
        prev = values[i];
    }

    return (size_t)(p - output);
}

/* Decode delta-encoded array back to unsigned absolute values */
size_t varintDeltaDecodeUnsigned(const uint8_t *input, size_t count,
                                 uint64_t *output) {
    if (count == 0) {
        return 0;
    }

    const uint8_t *p = input;

    /* Read base value */
    varintWidth baseWidth = (varintWidth)(*p++);
    uint64_t base = varintExternalGet(p, baseWidth);
    p += baseWidth;

    output[0] = base;

    /* Decode deltas and reconstruct absolute values */
    uint64_t current = base;
    for (size_t i = 1; i < count; i++) {
        int64_t delta;
        varintWidth deltaBytes = varintDeltaGet(p, &delta);
        p += deltaBytes;

        current = (uint64_t)((int64_t)current + delta);
        output[i] = current;
    }

    return (size_t)(p - input);
}
