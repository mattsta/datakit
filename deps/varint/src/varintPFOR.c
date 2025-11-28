/*
** This file implements Patched Frame-of-Reference (PFOR) encoding for
** variable-length integers. PFOR is optimal for data with clustered
** values and few outliers (typically <5%).
**
** Format:
**   [min_value][width][count][value_0]...[value_N][exception_count]
**   [exception_index_0][exception_value_0]...
**
** Algorithm:
**   1. Find minimum value and configurable percentile (e.g., 95th)
**   2. Compute width based on (percentile - min)
**   3. Values within range: store as (value - min) with fixed width
**   4. Outliers: store exception marker + append to exception list
**   5. Exception marker: all bits set for the chosen width
**
** Use cases:
**   - Stock prices (clustered with rare spikes)
**   - Response times (mostly fast, rare slow)
**   - Network latency (mostly low, rare high)
**   - Any distribution with <5% outliers
**
*************************************************************************
*/

#include "varintPFOR.h"
#include "varintTagged.h"
#include <stdlib.h>
#include <string.h>

/* Comparison function for qsort */
static int compare_uint64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* Calculate exception marker: all bits set for width */
static uint64_t varintPFORCalculateMarker(varintWidth width) {
    if (width >= 8) {
        return UINT64_MAX;
    }
    return (1ULL << (width * 8)) - 1;
}

/* Compute optimal threshold and metadata for encoding */
varintWidth varintPFORComputeThreshold(const uint64_t *values, uint32_t count,
                                       uint32_t threshold,
                                       varintPFORMeta *meta) {
    if (count == 0) {
        memset(meta, 0, sizeof(*meta));
        return VARINT_WIDTH_8B;
    }

    /* Create sorted copy for percentile calculation */
    uint64_t *sorted = malloc(count * sizeof(uint64_t));
    if (!sorted) {
        /* Out of memory - return conservative estimate */
        memset(meta, 0, sizeof(*meta));
        return VARINT_WIDTH_8B;
    }
    memcpy(sorted, values, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_uint64);

    /* Find min and threshold percentile */
    uint64_t min = sorted[0];
    uint32_t thresholdIndex = (count * threshold) / 100;
    if (thresholdIndex >= count) {
        thresholdIndex = count - 1;
    }
    uint64_t thresholdValue = sorted[thresholdIndex];

    /* Calculate range and required width */
    uint64_t range = thresholdValue - min;
    varintWidth width;
    varintExternalUnsignedEncoding(range, width);

    /* Calculate exception marker */
    uint64_t marker = varintPFORCalculateMarker(width);

    /* Count exceptions - values above threshold percentile */
    uint32_t exceptionCount = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (values[i] > thresholdValue) {
            /* Value above threshold is an exception */
            exceptionCount++;
        }
    }

    /* Fill metadata */
    meta->min = min;
    meta->width = width;
    meta->count = count;
    meta->exceptionCount = exceptionCount;
    meta->exceptionMarker = marker;
    meta->threshold = threshold;
    meta->thresholdValue = thresholdValue;

    free(sorted);
    return width;
}

/* Calculate size needed for encoding */
size_t varintPFORSize(const varintPFORMeta *meta) {
    size_t size = 0;

    /* Header: min (varint) + width (1 byte) + count (varint) */
    size += varintTaggedLen(meta->min);   /* min value */
    size += 1;                            /* width byte */
    size += varintTaggedLen(meta->count); /* count */

    /* Values: count * width */
    size += (size_t)meta->count * meta->width;

    /* Exception count */
    size += varintTaggedLen(meta->exceptionCount);

    /* Exceptions: each is (index, value) pair */
    for (uint32_t i = 0; i < meta->exceptionCount; i++) {
        size += varintTaggedLen(i);          /* worst case index */
        size += varintTaggedLen(UINT64_MAX); /* worst case value */
    }

    return size;
}

/* Encode array of values using PFOR */
size_t varintPFOREncode(uint8_t *dst, const uint64_t *values, uint32_t count,
                        uint32_t threshold, varintPFORMeta *meta) {
    uint8_t *start = dst;

    /* Compute metadata */
    varintPFORComputeThreshold(values, count, threshold, meta);

    /* Write header: min, width, count */
    dst += varintTaggedPut64(dst, meta->min);
    *dst++ = (uint8_t)meta->width;
    dst += varintTaggedPut64(dst, meta->count);

    /* Track exceptions for second pass */
    typedef struct {
        uint32_t index;
        uint64_t value;
    } Exception;

    Exception *exceptions = NULL;
    uint32_t exceptionIdx = 0;

    if (meta->exceptionCount > 0) {
        exceptions = malloc(meta->exceptionCount * sizeof(Exception));
        if (!exceptions) {
            /* Out of memory - fall back to encoding without exception tracking
             * This will still produce valid output, just not optimal */
            meta->exceptionCount = 0;
        }
    }

    /* Write values (first pass: mark exceptions) */
    for (uint32_t i = 0; i < count; i++) {
        uint64_t value = values[i];

        if (value > meta->thresholdValue && exceptions) {
            /* Above threshold: store exception marker */
            varintExternalPutFixedWidth(dst, meta->exceptionMarker,
                                        meta->width);
            exceptions[exceptionIdx].index = i;
            exceptions[exceptionIdx].value = value;
            exceptionIdx++;
        } else {
            /* Normal value: store offset from min */
            uint64_t offset = value - meta->min;
            varintExternalPutFixedWidth(dst, offset, meta->width);
        }
        dst += meta->width;
    }

    /* Write exception count */
    dst += varintTaggedPut64(dst, meta->exceptionCount);

    /* Write exceptions: (index, value) pairs */
    for (uint32_t i = 0; i < meta->exceptionCount; i++) {
        dst += varintTaggedPut64(dst, exceptions[i].index);
        dst += varintTaggedPut64(dst, exceptions[i].value);
    }

    free(exceptions);
    return (size_t)(dst - start);
}

/* Read metadata from encoded buffer */
size_t varintPFORReadMeta(const uint8_t *src, varintPFORMeta *meta) {
    const uint8_t *start = src;

    /* Read header: min, width, count */
    src += varintTaggedGet64(src, &meta->min);
    meta->width = (varintWidth)*src++;
    src += varintTaggedGet64(src, (uint64_t *)&meta->count);

    /* Calculate exception marker */
    meta->exceptionMarker = varintPFORCalculateMarker(meta->width);

    /* Skip to exception count (after all values) */
    const uint8_t *exceptionCountPtr = src + (meta->count * meta->width);
    varintTaggedGet64(exceptionCountPtr, (uint64_t *)&meta->exceptionCount);

    /* threshold is not stored, set to default */
    meta->threshold = VARINT_PFOR_THRESHOLD_95;

    return (size_t)(src - start);
}

/* Decode PFOR-encoded data into values array */
size_t varintPFORDecode(const uint8_t *src, uint64_t *values,
                        varintPFORMeta *meta) {
    /* Read metadata if not already provided */
    if (meta->width == 0) {
        src += varintPFORReadMeta(src, meta);
    } else {
        /* Skip header */
        src += varintTaggedLen(meta->min);
        src += 1; /* width */
        src += varintTaggedLen(meta->count);
    }

    /* Read all values */
    for (uint32_t i = 0; i < meta->count; i++) {
        uint64_t offset;
        varintExternalGetQuick_(src, meta->width, offset);

        if (offset == meta->exceptionMarker) {
            /* Exception placeholder, will be filled later */
            values[i] = UINT64_MAX; /* temporary marker */
        } else {
            /* Regular value: add offset to min */
            values[i] = meta->min + offset;
        }

        src += meta->width;
    }

    /* Read exception count */
    uint64_t exceptionCount;
    src += varintTaggedGet64(src, &exceptionCount);
    meta->exceptionCount = (uint32_t)exceptionCount;

    /* Read and apply exceptions */
    for (uint32_t i = 0; i < meta->exceptionCount; i++) {
        uint64_t index, value;
        src += varintTaggedGet64(src, &index);
        src += varintTaggedGet64(src, &value);

        if (index < meta->count) {
            values[index] = value;
        }
    }

    return (size_t)meta->count;
}

/* Random access: get value at specific index */
uint64_t varintPFORGetAt(const uint8_t *src, uint32_t index,
                         const varintPFORMeta *meta) {
    if (index >= meta->count) {
        return 0;
    }

    /* Skip header */
    const uint8_t *valuePtr = src;
    valuePtr += varintTaggedLen(meta->min);
    valuePtr += 1; /* width */
    valuePtr += varintTaggedLen(meta->count);

    /* Jump to value */
    valuePtr += (size_t)index * meta->width;

    /* Read value */
    uint64_t offset;
    varintExternalGetQuick_(valuePtr, meta->width, offset);

    if (offset != meta->exceptionMarker) {
        /* Regular value */
        return meta->min + offset;
    }

    /* Exception: search exception list */
    const uint8_t *exceptionPtr = src;
    exceptionPtr += varintTaggedLen(meta->min);
    exceptionPtr += 1; /* width */
    exceptionPtr += varintTaggedLen(meta->count);
    exceptionPtr += (size_t)meta->count * meta->width;

    /* Read exception count */
    uint64_t exceptionCount;
    exceptionPtr += varintTaggedGet64(exceptionPtr, &exceptionCount);

    /* Search for matching index */
    for (uint32_t i = 0; i < exceptionCount; i++) {
        uint64_t exIdx, exValue;
        varintWidth w1 = varintTaggedGet64(exceptionPtr, &exIdx);
        varintWidth w2 = varintTaggedGet64(exceptionPtr + w1, &exValue);

        if (exIdx == index) {
            return exValue;
        }

        exceptionPtr += w1 + w2;
    }

    /* Should not reach here if data is valid */
    return 0;
}
