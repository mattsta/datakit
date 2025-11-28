/**
 * varintAdaptive.c - Intelligent automatic encoding selection
 *
 * This implementation analyzes data characteristics and automatically
 * selects the most efficient encoding from: DELTA, FOR, PFOR, DICT,
 * BITMAP, or TAGGED.
 *
 * Analysis is performed in a single pass over the data, computing:
 * - Uniqueness ratio (for dictionary detection)
 * - Sortedness (for delta encoding)
 * - Range and clustering (for FOR/PFOR)
 * - Value distribution (for outlier detection)
 */

#include "varintAdaptive.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Check for overflow in size_t multiplication */
static inline bool size_mul_overflow(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return false;
    }
    *result = a * b;
    return (*result / a) != b;
}

/* ====================================================================
 * Helper Functions
 * ==================================================================== */

/* Check if array is sorted (ascending or descending) */
int varintAdaptiveCheckSorted(const uint64_t *values, size_t count) {
    if (count <= 1) {
        return 1; /* Single element is sorted */
    }

    bool ascending = true;
    bool descending = true;

    for (size_t i = 1; i < count; i++) {
        if (values[i] < values[i - 1]) {
            ascending = false;
        }
        if (values[i] > values[i - 1]) {
            descending = false;
        }

        /* Early exit if neither */
        if (!ascending && !descending) {
            return 0;
        }
    }

    if (ascending) {
        return 1;
    }
    if (descending) {
        return -1;
    }
    return 0;
}

/* Count unique values using simple sorting approach */
size_t varintAdaptiveCountUnique(const uint64_t *values, size_t count) {
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return 1;
    }

    /* For large arrays, use sampling to avoid expensive full sort */
    if (count > 10000) {
        /* Sample-based estimation: check every 10th element */
        size_t sampleSize = count / 10;
        if (sampleSize < 100) {
            sampleSize = 100; /* Ensure minimum sample size */
        }

        size_t allocSize;
        if (size_mul_overflow(sampleSize, sizeof(uint64_t), &allocSize)) {
            return count; /* Integer overflow, conservative estimate */
        }

        uint64_t *sample = malloc(allocSize);
        if (!sample) {
            return count; /* Conservative estimate */
        }

        /* Collect samples */
        size_t step = count / sampleSize;
        for (size_t i = 0; i < sampleSize; i++) {
            sample[i] = values[i * step];
        }

        /* Sort sample */
        for (size_t i = 0; i < sampleSize - 1; i++) {
            for (size_t j = i + 1; j < sampleSize; j++) {
                if (sample[i] > sample[j]) {
                    uint64_t tmp = sample[i];
                    sample[i] = sample[j];
                    sample[j] = tmp;
                }
            }
        }

        /* Count unique in sample */
        size_t uniqueInSample = 1;
        for (size_t i = 1; i < sampleSize; i++) {
            if (sample[i] != sample[i - 1]) {
                uniqueInSample++;
            }
        }

        free(sample);

        /* Extrapolate to full dataset */
        size_t estimated = (uniqueInSample * count) / sampleSize;
        return estimated > count ? count : estimated;
    }

    /* For smaller arrays, do exact count with full sort */
    size_t allocSize;
    if (size_mul_overflow(count, sizeof(uint64_t), &allocSize)) {
        return count; /* Integer overflow, conservative estimate */
    }

    uint64_t *sorted = malloc(allocSize);
    if (!sorted) {
        return count; /* Conservative estimate */
    }

    memcpy(sorted, values, count * sizeof(uint64_t));

    /* Simple bubble sort (good enough for small arrays) */
    for (size_t i = 0; i < count - 1; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (sorted[i] > sorted[j]) {
                uint64_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    /* Count unique values */
    size_t unique = 1;
    for (size_t i = 1; i < count; i++) {
        if (sorted[i] != sorted[i - 1]) {
            unique++;
        }
    }

    free(sorted);
    return unique;
}

/* Compute average absolute delta */
uint64_t varintAdaptiveAvgDelta(const uint64_t *values, size_t count) {
    if (count <= 1) {
        return 0;
    }

    uint64_t totalDelta = 0;
    for (size_t i = 1; i < count; i++) {
        uint64_t delta = values[i] > values[i - 1] ? values[i] - values[i - 1]
                                                   : values[i - 1] - values[i];
        totalDelta += delta;
    }

    return totalDelta / (count - 1);
}

/* ====================================================================
 * Analysis Functions
 * ==================================================================== */

void varintAdaptiveAnalyze(const uint64_t *values, size_t count,
                           varintAdaptiveDataStats *stats) {
    memset(stats, 0, sizeof(varintAdaptiveDataStats));

    if (count == 0) {
        return;
    }

    stats->count = count;
    stats->minValue = values[0];
    stats->maxValue = values[0];

    /* Single pass to compute min, max, and check sortedness */
    for (size_t i = 1; i < count; i++) {
        if (values[i] < stats->minValue) {
            stats->minValue = values[i];
        }
        if (values[i] > stats->maxValue) {
            stats->maxValue = values[i];
        }
    }

    stats->range = stats->maxValue - stats->minValue;
    stats->fitsInBitmapRange = (stats->maxValue < VARINT_BITMAP_MAX_VALUE);

    /* Check sortedness */
    int sortedness = varintAdaptiveCheckSorted(values, count);
    stats->isSorted = (sortedness == 1);
    stats->isReverseSorted = (sortedness == -1);

    /* Count unique values (may be approximate for large arrays) */
    stats->uniqueCount = varintAdaptiveCountUnique(values, count);
    stats->uniqueRatio = (float)stats->uniqueCount / (float)count;

    /* Compute delta statistics */
    stats->avgDelta = varintAdaptiveAvgDelta(values, count);

    /* Find max delta */
    stats->maxDelta = 0;
    for (size_t i = 1; i < count; i++) {
        uint64_t delta = values[i] > values[i - 1] ? values[i] - values[i - 1]
                                                   : values[i - 1] - values[i];
        if (delta > stats->maxDelta) {
            stats->maxDelta = delta;
        }
    }

    /* Compute outlier statistics (values beyond 95th percentile of range) */
    if (stats->range > 0) {
        /* Simple outlier detection: values in top 5% of range */
        uint64_t threshold95 = stats->minValue + (stats->range * 95) / 100;
        stats->outlierCount = 0;
        for (size_t i = 0; i < count; i++) {
            if (values[i] > threshold95) {
                stats->outlierCount++;
            }
        }
        stats->outlierRatio = (float)stats->outlierCount / (float)count;
    }
}

/* Select optimal encoding based on statistics */
varintAdaptiveEncodingType
varintAdaptiveSelectEncoding(const varintAdaptiveDataStats *stats) {
    /* Edge cases */
    if (stats->count == 0) {
        return VARINT_ADAPTIVE_TAGGED;
    }

    if (stats->count == 1) {
        return VARINT_ADAPTIVE_TAGGED; /* Single value, just use tagged */
    }

    /* Decision tree based on data characteristics */

    /* 1. High repetition → Dictionary encoding
     * If less than 15% unique values, dictionary is very efficient */
    if (stats->uniqueRatio < 0.15f) {
        return VARINT_ADAPTIVE_DICT;
    }

    /* 2. Dense sets in bitmap range → Bitmap encoding
     * IMPORTANT: Bitmap is for SETS (unique values only), not sequences
     * Only use if all values are unique or nearly unique
     * AND data is already sorted (since BITMAP returns values in sorted order)
     */
    if (stats->fitsInBitmapRange && stats->uniqueRatio > 0.9f &&
        (stats->isSorted || stats->isReverseSorted)) {
        /* All or nearly all values are unique - bitmap might work */
        if (stats->range > 0 && stats->count < 10000) {
            float density = (float)stats->count / (float)stats->range;
            if (density > 0.05f) {
                return VARINT_ADAPTIVE_BITMAP;
            }
        }
    }

    /* 3. Sorted with small deltas → Delta encoding
     * Delta encoding excels when values are sequential or sorted
     * and deltas are small relative to absolute values */
    if (stats->isSorted || stats->isReverseSorted) {
        /* Good for delta if average delta is much smaller than values */
        if (stats->minValue > 0 && stats->avgDelta < stats->minValue / 10) {
            return VARINT_ADAPTIVE_DELTA;
        }
        /* Also good if deltas are just small in absolute terms */
        if (stats->avgDelta < 1000) {
            return VARINT_ADAPTIVE_DELTA;
        }
    }

    /* 4. Clustered with few outliers → PFOR encoding
     * If < 5% outliers and range is moderate, PFOR handles this well */
    if (stats->outlierRatio < 0.05f && stats->range > 0) {
        /* PFOR makes sense if most values cluster in a small range */
        return VARINT_ADAPTIVE_PFOR;
    }

    /* 5. Clustered values with small range → FOR encoding
     * If range is small relative to count, FOR is efficient */
    if (stats->range > 0 && stats->range < stats->count * 100) {
        return VARINT_ADAPTIVE_FOR;
    }

    /* 6. Fallback → Tagged encoding
     * For sparse, random, or wide-ranging data, tagged is most reliable */
    return VARINT_ADAPTIVE_TAGGED;
}

/* ====================================================================
 * Encoding Functions
 * ==================================================================== */

size_t varintAdaptiveEncodeWith(uint8_t *dst, const uint64_t *values,
                                size_t count,
                                varintAdaptiveEncodingType encodingType,
                                varintAdaptiveMeta *meta) {
    /* Validate parameters */
    if (!dst || !values) {
        return 0;
    }

    /* Write encoding type header */
    dst[0] = (uint8_t)encodingType;
    size_t offset = 1;

    /* Encode with selected encoding */
    size_t encodedSize = 0;

    switch (encodingType) {
    case VARINT_ADAPTIVE_DELTA: {
        /* Delta encoding for signed values - convert to signed */
        size_t allocSize;
        if (size_mul_overflow(count, sizeof(int64_t), &allocSize)) {
            return 0; /* Integer overflow */
        }

        int64_t *signedValues = malloc(allocSize);
        if (!signedValues) {
            return 0;
        }

        for (size_t i = 0; i < count; i++) {
            signedValues[i] = (int64_t)values[i];
        }

        encodedSize = varintDeltaEncodeUnsigned(dst + offset, values, count);
        free(signedValues);
        break;
    }

    case VARINT_ADAPTIVE_FOR: {
        varintFORMeta forMeta;
        encodedSize = varintFOREncode(dst + offset, values, count, &forMeta);

        if (meta) {
            meta->encodingMeta.forMeta = forMeta;
        }
        break;
    }

    case VARINT_ADAPTIVE_PFOR: {
        varintPFORMeta pforMeta;
        encodedSize = varintPFOREncode(dst + offset, values, (uint32_t)count,
                                       VARINT_PFOR_THRESHOLD_95, &pforMeta);

        if (meta) {
            meta->encodingMeta.pforMeta = pforMeta;
        }
        break;
    }

    case VARINT_ADAPTIVE_DICT: {
        encodedSize = varintDictEncode(dst + offset, values, count);
        break;
    }

    case VARINT_ADAPTIVE_BITMAP: {
        /* Convert to bitmap - only works if values fit in uint16_t range */
        varintBitmap *vb = varintBitmapCreate();
        if (!vb) {
            return 0;
        }

        for (size_t i = 0; i < count; i++) {
            if (values[i] < VARINT_BITMAP_MAX_VALUE) {
                varintBitmapAdd(vb, (uint16_t)values[i]);
            }
        }

        encodedSize = varintBitmapEncode(vb, dst + offset);
        varintBitmapFree(vb);
        break;
    }

    case VARINT_ADAPTIVE_TAGGED:
    default: {
        /* Tagged encoding - simple but reliable fallback */
        for (size_t i = 0; i < count; i++) {
            varintWidth width = varintTaggedPut64(dst + offset, values[i]);
            offset += width;
        }
        encodedSize = offset - 1; /* Subtract initial header byte */
        break;
    }
    }

    /* Fill metadata if requested */
    if (meta) {
        meta->encodingType = encodingType;
        meta->originalCount = count;
        meta->encodedSize = encodedSize + 1; /* +1 for header byte */
    }

    return encodedSize + 1; /* +1 for encoding type header */
}

size_t varintAdaptiveEncode(uint8_t *dst, const uint64_t *values, size_t count,
                            varintAdaptiveMeta *meta) {
    /* Analyze data to select encoding */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(values, count, &stats);

    /* Select optimal encoding */
    varintAdaptiveEncodingType encodingType =
        varintAdaptiveSelectEncoding(&stats);

    /* Encode with selected encoding */
    return varintAdaptiveEncodeWith(dst, values, count, encodingType, meta);
}

/* ====================================================================
 * Decoding Functions
 * ==================================================================== */

size_t varintAdaptiveDecode(const uint8_t *src, uint64_t *values,
                            size_t maxCount, varintAdaptiveMeta *meta) {
    /* Validate parameters */
    if (!src || !values) {
        return 0;
    }

    /* Read encoding type from header */
    varintAdaptiveEncodingType encodingType =
        (varintAdaptiveEncodingType)src[0];
    const uint8_t *data = src + 1;

    size_t decoded = 0;

    switch (encodingType) {
    case VARINT_ADAPTIVE_DELTA: {
        /* varintDeltaDecodeUnsigned returns bytes read, not values decoded */
        varintDeltaDecodeUnsigned(data, maxCount, values);
        decoded = maxCount;
        break;
    }

    case VARINT_ADAPTIVE_FOR: {
        decoded = varintFORDecode(data, values, maxCount);
        break;
    }

    case VARINT_ADAPTIVE_PFOR: {
        varintPFORMeta pforMeta;
        varintPFORReadMeta(data, &pforMeta);
        decoded = varintPFORDecode(data, values, &pforMeta);

        if (meta) {
            meta->encodingMeta.pforMeta = pforMeta;
        }
        break;
    }

    case VARINT_ADAPTIVE_DICT: {
        /* Dict encoding is self-describing, pass large buffer size */
        decoded = varintDictDecodeInto(data, 1024 * 1024, values, maxCount);
        break;
    }

    case VARINT_ADAPTIVE_BITMAP: {
        /* Bitmap encoding is self-describing, pass large buffer size */
        varintBitmap *vb = varintBitmapDecode(data, 1024 * 1024);
        if (vb) {
            /* Extract values from bitmap */
            size_t allocSize;
            uint16_t *shortValues = NULL;
            if (!size_mul_overflow(maxCount, sizeof(uint16_t), &allocSize)) {
                shortValues = malloc(allocSize);
            }

            if (shortValues) {
                uint32_t count = varintBitmapToArray(vb, shortValues);
                decoded = count < maxCount ? count : maxCount;

                for (size_t i = 0; i < decoded; i++) {
                    values[i] = shortValues[i];
                }

                free(shortValues);
            }
            varintBitmapFree(vb);
        }
        break;
    }

    case VARINT_ADAPTIVE_TAGGED:
    default: {
        /* Decode tagged varints */
        size_t offset = 0;
        size_t count = 0;

        while (count < maxCount && offset < maxCount * 9) {
            uint64_t value;
            varintWidth width = varintTaggedGet64(data + offset, &value);
            if (width == 0) {
                break;
            }

            values[count++] = value;
            offset += width;
        }

        decoded = count;
        break;
    }
    }

    /* Fill metadata if requested */
    if (meta) {
        meta->encodingType = encodingType;
        meta->originalCount = decoded;
    }

    return decoded;
}

/* ====================================================================
 * Metadata Functions
 * ==================================================================== */

size_t varintAdaptiveReadMeta(const uint8_t *src, varintAdaptiveMeta *meta) {
    meta->encodingType = (varintAdaptiveEncodingType)src[0];

    /* Read encoding-specific metadata */
    const uint8_t *data = src + 1;

    switch (meta->encodingType) {
    case VARINT_ADAPTIVE_FOR: {
        varintFORReadMetadata(data, &meta->encodingMeta.forMeta);
        meta->originalCount = meta->encodingMeta.forMeta.count;
        meta->encodedSize = meta->encodingMeta.forMeta.encodedSize + 1;
        break;
    }

    case VARINT_ADAPTIVE_PFOR: {
        varintPFORReadMeta(data, &meta->encodingMeta.pforMeta);
        meta->originalCount = meta->encodingMeta.pforMeta.count;
        meta->encodedSize = varintPFORSize(&meta->encodingMeta.pforMeta) + 1;
        break;
    }

    default:
        /* Other encodings don't have easily extractable metadata */
        meta->originalCount = 0;
        meta->encodedSize = 1; /* At least header byte */
        break;
    }

    return 1; /* Header size */
}

const char *varintAdaptiveEncodingName(varintAdaptiveEncodingType type) {
    switch (type) {
    case VARINT_ADAPTIVE_DELTA:
        return "DELTA";
    case VARINT_ADAPTIVE_FOR:
        return "FOR";
    case VARINT_ADAPTIVE_PFOR:
        return "PFOR";
    case VARINT_ADAPTIVE_DICT:
        return "DICT";
    case VARINT_ADAPTIVE_BITMAP:
        return "BITMAP";
    case VARINT_ADAPTIVE_TAGGED:
        return "TAGGED";
    case VARINT_ADAPTIVE_GROUP:
        return "GROUP";
    default:
        return "UNKNOWN";
    }
}
