/*
** This file contains routines for dictionary encoding of repetitive
** uint64_t values. Dictionary encoding is optimal for data with low
** cardinality but high repetition (e.g., log sources, enums, status codes).
**
** Format: [dict_size][dict_entries...][count][indices...]
**   - dict_size: varintTagged (number of unique values)
**   - dict_entries: varintTagged for each unique value
**   - count: varintTagged (number of values in original array)
**   - indices: varintExternal for each index (width based on dict_size)
**
** Compression efficiency:
**   - Best: 10 unique values across 1M entries = 99%+ savings
**   - Good: < 10% unique values = significant savings
**   - Poor: > 50% unique values = potential expansion
**
*************************************************************************
*/

#include "varintDict.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Maximum dictionary size to prevent DoS via excessive memory allocation */
#define VARINT_DICT_MAX_SIZE 1048576 /* 1M entries = 8MB for dict values */

/* Check for overflow in size_t multiplication */
static inline bool size_mul_overflow(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return false;
    }
    *result = a * b;
    return (*result / a) != b;
}

/* Internal comparison function for qsort */
static int compareUint64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

/* Binary search for value in sorted dictionary */
static int32_t binarySearch(const uint64_t *values, const uint32_t size,
                            const uint64_t target) {
    int32_t left = 0;
    int32_t right = (int32_t)(size - 1);

    while (left <= right) {
        int32_t mid = left + (right - left) / 2;
        if (values[mid] == target) {
            return mid;
        } else if (values[mid] < target) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    return -1; /* Not found */
}

/* ====================================================================
 * Dictionary Management
 * ==================================================================== */

varintDict *varintDictCreate(void) {
    varintDict *dict = (varintDict *)calloc(1, sizeof(varintDict));
    if (!dict) {
        return NULL;
    }
    dict->capacity = 16; /* Start with small capacity */
    dict->values = (uint64_t *)malloc(dict->capacity * sizeof(uint64_t));
    if (!dict->values) {
        free(dict);
        return NULL;
    }
    return dict;
}

void varintDictFree(varintDict *dict) {
    if (dict) {
        free(dict->values);
        free(dict);
    }
}

int varintDictBuild(varintDict *dict, const uint64_t *values, size_t count) {
    if (!dict || !values || count == 0) {
        return -1;
    }

    /* Allocate temporary array for sorting */
    uint64_t *sorted = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!sorted) {
        return -1;
    }
    memcpy(sorted, values, count * sizeof(uint64_t));

    /* Sort to find unique values */
    qsort(sorted, count, sizeof(uint64_t), compareUint64);

    /* Count unique values */
    uint32_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || sorted[i] != sorted[i - 1]) {
            unique++;
        }
    }

    /* Ensure dictionary has enough capacity */
    if (unique > dict->capacity) {
        uint64_t *newValues =
            (uint64_t *)realloc(dict->values, unique * sizeof(uint64_t));
        if (!newValues) {
            free(sorted);
            return -1;
        }
        dict->values = newValues;
        dict->capacity = unique;
    }

    /* Extract unique values */
    dict->size = 0;
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || sorted[i] != sorted[i - 1]) {
            dict->values[dict->size++] = sorted[i];
        }
    }

    free(sorted);

    /* Calculate index width needed */
    if (dict->size == 0) {
        dict->indexWidth = VARINT_WIDTH_8B;
    } else {
        uint64_t maxIndex = dict->size - 1;
        varintExternalUnsignedEncoding(maxIndex, dict->indexWidth);
    }

    return 0;
}

int32_t varintDictFind(const varintDict *dict, const uint64_t value) {
    if (!dict || dict->size == 0) {
        return -1;
    }
    return binarySearch(dict->values, dict->size, value);
}

uint64_t varintDictLookup(const varintDict *dict, const uint32_t index) {
    if (!dict || index >= dict->size) {
        return 0;
    }
    return dict->values[index];
}

/* ====================================================================
 * Encoding and Decoding
 * ==================================================================== */

size_t varintDictEncode(uint8_t *buffer, const uint64_t *values, size_t count) {
    if (!buffer || !values || count == 0) {
        return 0;
    }

    /* Build dictionary */
    varintDict *dict = varintDictCreate();
    if (!dict) {
        return 0;
    }

    if (varintDictBuild(dict, values, count) != 0) {
        varintDictFree(dict);
        return 0;
    }

    size_t written = varintDictEncodeWithDict(buffer, dict, values, count);
    varintDictFree(dict);
    return written;
}

size_t varintDictEncodeWithDict(uint8_t *buffer, const varintDict *dict,
                                const uint64_t *values, size_t count) {
    if (!buffer || !dict || !values || count == 0) {
        return 0;
    }

    uint8_t *ptr = buffer;

    /* Write dictionary size */
    varintWidth w = varintTaggedPut64(ptr, dict->size);
    ptr += w;

    /* Write dictionary entries */
    for (uint32_t i = 0; i < dict->size; i++) {
        w = varintTaggedPut64(ptr, dict->values[i]);
        ptr += w;
    }

    /* Write count */
    w = varintTaggedPut64(ptr, count);
    ptr += w;

    /* Write indices */
    for (size_t i = 0; i < count; i++) {
        int32_t index = varintDictFind(dict, values[i]);
        if (index < 0) {
            return 0; /* Value not in dictionary */
        }
        varintExternalPutFixedWidthQuick_(ptr, (uint64_t)index,
                                          dict->indexWidth);
        ptr += dict->indexWidth;
    }

    return (size_t)(ptr - buffer);
}

uint64_t *varintDictDecode(const uint8_t *buffer, size_t bufferLen,
                           size_t *outCount) {
    if (!buffer || bufferLen == 0 || !outCount) {
        return NULL;
    }

    const uint8_t *ptr = buffer;
    const uint8_t *end = buffer + bufferLen;

    /* Read dictionary size */
    uint64_t dictSize64;
    varintWidth w = varintTaggedGet64(ptr, &dictSize64);
    if (w == 0 || ptr + w > end) {
        return NULL;
    }
    ptr += w;

    /* Validate dictionary size to prevent DoS and integer overflow */
    if (dictSize64 > VARINT_DICT_MAX_SIZE) {
        return NULL; /* Dictionary too large */
    }
    uint32_t dictSize = (uint32_t)dictSize64;

    /* Check for overflow in size calculation */
    size_t allocSize;
    if (size_mul_overflow(dictSize, sizeof(uint64_t), &allocSize)) {
        return NULL; /* Integer overflow in allocation size */
    }

    /* Read dictionary entries */
    uint64_t *dictValues = (uint64_t *)malloc(allocSize);
    if (!dictValues) {
        return NULL;
    }

    for (uint32_t i = 0; i < dictSize; i++) {
        w = varintTaggedGet64(ptr, &dictValues[i]);
        if (w == 0 || ptr + w > end) {
            free(dictValues);
            return NULL;
        }
        ptr += w;
    }

    /* Read count */
    uint64_t count64;
    w = varintTaggedGet64(ptr, &count64);
    if (w == 0 || ptr + w > end) {
        free(dictValues);
        return NULL;
    }
    ptr += w;
    size_t count = (size_t)count64;

    /* Determine index width */
    varintWidth indexWidth;
    if (dictSize == 0) {
        indexWidth = VARINT_WIDTH_8B;
    } else {
        uint64_t maxIndex = dictSize - 1;
        varintExternalUnsignedEncoding(maxIndex, indexWidth);
    }

    /* Check if we have enough buffer for indices */
    if (ptr + (count * indexWidth) > end) {
        free(dictValues);
        return NULL;
    }

    /* Allocate output array */
    uint64_t *output = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!output) {
        free(dictValues);
        return NULL;
    }

    /* Decode indices */
    for (size_t i = 0; i < count; i++) {
        uint64_t index;
        varintExternalGetQuick_(ptr, indexWidth, index);
        if (index >= dictSize) {
            free(dictValues);
            free(output);
            return NULL;
        }
        output[i] = dictValues[index];
        ptr += indexWidth;
    }

    free(dictValues);
    *outCount = count;
    return output;
}

size_t varintDictDecodeInto(const uint8_t *buffer, size_t bufferLen,
                            uint64_t *output, size_t maxValues) {
    if (!buffer || bufferLen == 0 || !output || maxValues == 0) {
        return 0;
    }

    const uint8_t *ptr = buffer;
    const uint8_t *end = buffer + bufferLen;

    /* Read dictionary size */
    uint64_t dictSize64;
    varintWidth w = varintTaggedGet64(ptr, &dictSize64);
    if (w == 0 || ptr + w > end) {
        return 0;
    }
    ptr += w;

    /* Validate dictionary size to prevent DoS and integer overflow */
    if (dictSize64 > VARINT_DICT_MAX_SIZE) {
        return 0; /* Dictionary too large */
    }
    uint32_t dictSize = (uint32_t)dictSize64;

    /* Check for overflow in size calculation */
    size_t allocSize;
    if (size_mul_overflow(dictSize, sizeof(uint64_t), &allocSize)) {
        return 0; /* Integer overflow in allocation size */
    }

    /* Read dictionary entries */
    uint64_t *dictValues = (uint64_t *)malloc(allocSize);
    if (!dictValues) {
        return 0;
    }

    for (uint32_t i = 0; i < dictSize; i++) {
        w = varintTaggedGet64(ptr, &dictValues[i]);
        if (w == 0 || ptr + w > end) {
            free(dictValues);
            return 0;
        }
        ptr += w;
    }

    /* Read count */
    uint64_t count64;
    w = varintTaggedGet64(ptr, &count64);
    if (w == 0 || ptr + w > end) {
        free(dictValues);
        return 0;
    }
    ptr += w;
    size_t count = (size_t)count64;

    /* Check against max values */
    if (count > maxValues) {
        free(dictValues);
        return 0;
    }

    /* Determine index width */
    varintWidth indexWidth;
    if (dictSize == 0) {
        indexWidth = VARINT_WIDTH_8B;
    } else {
        uint64_t maxIndex = dictSize - 1;
        varintExternalUnsignedEncoding(maxIndex, indexWidth);
    }

    /* Check buffer bounds */
    if (ptr + (count * indexWidth) > end) {
        free(dictValues);
        return 0;
    }

    /* Decode indices */
    for (size_t i = 0; i < count; i++) {
        uint64_t index;
        varintExternalGetQuick_(ptr, indexWidth, index);
        if (index >= dictSize) {
            free(dictValues);
            return 0;
        }
        output[i] = dictValues[index];
        ptr += indexWidth;
    }

    free(dictValues);
    return count;
}

/* ====================================================================
 * Size Calculation and Analysis
 * ==================================================================== */

size_t varintDictEncodedSize(const uint64_t *values, size_t count) {
    if (!values || count == 0) {
        return 0;
    }

    /* Build dictionary to get accurate size */
    varintDict *dict = varintDictCreate();
    if (!dict) {
        return 0;
    }

    if (varintDictBuild(dict, values, count) != 0) {
        varintDictFree(dict);
        return 0;
    }

    size_t size = varintDictEncodedSizeWithDict(dict, count);
    varintDictFree(dict);
    return size;
}

size_t varintDictEncodedSizeWithDict(const varintDict *dict,
                                     const size_t count) {
    if (!dict || count == 0) {
        return 0;
    }

    size_t size = 0;

    /* Dictionary size */
    size += varintTaggedLen(dict->size);

    /* Dictionary entries */
    for (uint32_t i = 0; i < dict->size; i++) {
        size += varintTaggedLen(dict->values[i]);
    }

    /* Count */
    size += varintTaggedLen(count);

    /* Indices */
    size += count * dict->indexWidth;

    return size;
}

float varintDictCompressionRatio(const uint64_t *values, size_t count) {
    if (!values || count == 0) {
        return 0.0f;
    }

    size_t encodedSize = varintDictEncodedSize(values, count);
    if (encodedSize == 0) {
        return 0.0f;
    }

    size_t originalSize = count * sizeof(uint64_t);
    return (float)originalSize / (float)encodedSize;
}

int varintDictGetStats(const uint64_t *values, size_t count,
                       varintDictStats *stats) {
    if (!values || count == 0 || !stats) {
        return -1;
    }

    /* Build dictionary */
    varintDict *dict = varintDictCreate();
    if (!dict) {
        return -1;
    }

    if (varintDictBuild(dict, values, count) != 0) {
        varintDictFree(dict);
        return -1;
    }

    /* Calculate dictionary size */
    size_t dictBytes = varintTaggedLen(dict->size);
    for (uint32_t i = 0; i < dict->size; i++) {
        dictBytes += varintTaggedLen(dict->values[i]);
    }

    /* Calculate index size */
    size_t indexBytes = count * dict->indexWidth;

    /* Fill stats */
    stats->uniqueCount = dict->size;
    stats->totalCount = count;
    stats->dictBytes = dictBytes;
    stats->indexBytes = indexBytes;
    stats->totalBytes = dictBytes + varintTaggedLen(count) + indexBytes;
    stats->originalBytes = count * sizeof(uint64_t);
    stats->compressionRatio =
        (float)stats->originalBytes / (float)stats->totalBytes;
    stats->spaceReduction =
        (1.0f - (float)stats->totalBytes / (float)stats->originalBytes) *
        100.0f;

    varintDictFree(dict);
    return 0;
}
