#include "varintBitmap.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Internal helper functions
 * ==================================================================== */

/* Binary search in sorted array */
static int32_t binarySearch_(const uint16_t *array, uint32_t length,
                             uint16_t value) {
    if (length == 0) {
        return -1;
    }

    int32_t low = 0;
    int32_t high = (int32_t)(length - 1);

    while (low <= high) {
        int32_t mid = (low + high) / 2;
        uint16_t midVal = array[mid];

        if (midVal < value) {
            low = mid + 1;
        } else if (midVal > value) {
            high = mid - 1;
        } else {
            return mid; /* Found */
        }
    }

    return -(low + 1); /* Not found, return insertion point */
}

/* Count set bits in bitmap - used for validation/debugging */
__attribute__((unused)) static uint32_t
bitmapCardinality_(const uint8_t *bits) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < VARINT_BITMAP_BITMAP_SIZE; i++) {
        count += (uint32_t)__builtin_popcount(bits[i]);
    }
    return count;
}

/* Check if bit is set in bitmap */
static inline bool bitmapContains_(const uint8_t *bits, uint16_t value) {
    uint32_t byteIdx = value / 8;
    uint8_t bitIdx = value % 8;
    return (bits[byteIdx] & (1 << bitIdx)) != 0;
}

/* Set bit in bitmap, return true if changed */
static bool bitmapSet_(uint8_t *bits, uint16_t value) {
    uint32_t byteIdx = value / 8;
    uint8_t bitIdx = value % 8;
    uint8_t mask = (uint8_t)(1U << bitIdx);
    bool wasSet = (bits[byteIdx] & mask) != 0;
    bits[byteIdx] |= mask;
    return !wasSet;
}

/* Clear bit in bitmap, return true if changed */
static bool bitmapClear_(uint8_t *bits, uint16_t value) {
    uint32_t byteIdx = value / 8;
    uint8_t bitIdx = value % 8;
    uint8_t mask = (uint8_t)(1U << bitIdx);
    bool wasSet = (bits[byteIdx] & mask) != 0;
    bits[byteIdx] &= ~mask;
    return wasSet;
}

/* Convert array to bitmap container */
static bool arrayToBitmap_(varintBitmap *vb) {
    assert(vb->type == VARINT_BITMAP_ARRAY);

    uint8_t *bits = calloc(VARINT_BITMAP_BITMAP_SIZE, 1);
    if (!bits) {
        return false; /* Out of memory */
    }

    /* Set bits for all values in array */
    for (uint32_t i = 0; i < vb->cardinality; i++) {
        bitmapSet_(bits, vb->container.array.values[i]);
    }

    /* Free old array */
    free(vb->container.array.values);

    /* Update to bitmap */
    vb->type = VARINT_BITMAP_BITMAP;
    vb->container.bitmap.bits = bits;
    return true;
}

/* Convert bitmap to array container */
static bool bitmapToArray_(varintBitmap *vb) {
    assert(vb->type == VARINT_BITMAP_BITMAP);

    uint16_t *values = malloc(vb->cardinality * sizeof(uint16_t));
    if (!values) {
        return false; /* Out of memory */
    }

    /* Extract all set bits */
    uint32_t pos = 0;
    for (uint32_t i = 0; i < VARINT_BITMAP_MAX_VALUE; i++) {
        if (bitmapContains_(vb->container.bitmap.bits, (uint16_t)i)) {
            values[pos++] = (uint16_t)i;
        }
    }

    assert(pos == vb->cardinality);

    /* Free old bitmap */
    free(vb->container.bitmap.bits);

    /* Update to array */
    vb->type = VARINT_BITMAP_ARRAY;
    vb->container.array.values = values;
    vb->container.array.capacity = vb->cardinality;
    return true;
}

/* Ensure array has space for one more element */
static bool arrayEnsureCapacity_(varintBitmap *vb, uint32_t needed) {
    assert(vb->type == VARINT_BITMAP_ARRAY);

    if (vb->container.array.capacity >= needed) {
        return true;
    }

    uint32_t newCapacity = vb->container.array.capacity * 2;
    if (newCapacity < needed) {
        newCapacity = needed;
    }

    uint16_t *newValues =
        realloc(vb->container.array.values, newCapacity * sizeof(uint16_t));
    if (!newValues) {
        return false; /* Out of memory - original array still valid */
    }
    vb->container.array.values = newValues;
    vb->container.array.capacity = newCapacity;
    return true;
}

/* ====================================================================
 * Core API Implementation
 * ==================================================================== */

varintBitmap *varintBitmapCreate(void) {
    varintBitmap *vb = calloc(1, sizeof(varintBitmap));
    if (!vb) {
        return NULL; /* Out of memory */
    }

    /* Start with array container */
    vb->type = VARINT_BITMAP_ARRAY;
    vb->cardinality = 0;
    vb->container.array.values =
        malloc(VARINT_BITMAP_DEFAULT_ARRAY_CAPACITY * sizeof(uint16_t));
    if (!vb->container.array.values) {
        free(vb);
        return NULL; /* Out of memory */
    }
    vb->container.array.capacity = VARINT_BITMAP_DEFAULT_ARRAY_CAPACITY;

    return vb;
}

void varintBitmapFree(varintBitmap *vb) {
    if (vb == NULL) {
        return;
    }

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        free(vb->container.array.values);
        break;
    case VARINT_BITMAP_BITMAP:
        free(vb->container.bitmap.bits);
        break;
    case VARINT_BITMAP_RUNS:
        free(vb->container.runs.runs);
        break;
    }

    free(vb);
}

varintBitmap *varintBitmapClone(const varintBitmap *vb) {
    varintBitmap *clone = malloc(sizeof(varintBitmap));
    if (!clone) {
        return NULL; /* Out of memory */
    }

    clone->type = vb->type;
    clone->cardinality = vb->cardinality;

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        clone->container.array.capacity = vb->container.array.capacity;
        clone->container.array.values =
            malloc(vb->container.array.capacity * sizeof(uint16_t));
        if (!clone->container.array.values) {
            free(clone);
            return NULL; /* Out of memory */
        }
        memcpy(clone->container.array.values, vb->container.array.values,
               vb->cardinality * sizeof(uint16_t));
        break;

    case VARINT_BITMAP_BITMAP:
        clone->container.bitmap.bits = malloc(VARINT_BITMAP_BITMAP_SIZE);
        if (!clone->container.bitmap.bits) {
            free(clone);
            return NULL; /* Out of memory */
        }
        memcpy(clone->container.bitmap.bits, vb->container.bitmap.bits,
               VARINT_BITMAP_BITMAP_SIZE);
        break;

    case VARINT_BITMAP_RUNS:
        clone->container.runs.numRuns = vb->container.runs.numRuns;
        clone->container.runs.capacity = vb->container.runs.capacity;
        clone->container.runs.runs =
            malloc(vb->container.runs.capacity * 2 * sizeof(uint16_t));
        if (!clone->container.runs.runs) {
            free(clone);
            return NULL; /* Out of memory */
        }
        memcpy(clone->container.runs.runs, vb->container.runs.runs,
               vb->container.runs.numRuns * 2 * sizeof(uint16_t));
        break;
    }

    return clone;
}

bool varintBitmapAdd(varintBitmap *vb, uint16_t value) {
    switch (vb->type) {
    case VARINT_BITMAP_ARRAY: {
        /* Check if already present */
        int32_t idx =
            binarySearch_(vb->container.array.values, vb->cardinality, value);
        if (idx >= 0) {
            return false; /* Already present */
        }

        /* Check if we should convert to bitmap */
        if (vb->cardinality >= VARINT_BITMAP_ARRAY_MAX) {
            if (!arrayToBitmap_(vb)) {
                return false; /* Out of memory */
            }
            bitmapSet_(vb->container.bitmap.bits, value);
            vb->cardinality++;
            return true;
        }

        /* Insert into sorted array */
        int32_t insertPos = -(idx + 1);
        if (!arrayEnsureCapacity_(vb, vb->cardinality + 1)) {
            return false; /* Out of memory */
        }

        /* Shift elements right */
        memmove(&vb->container.array.values[insertPos + 1],
                &vb->container.array.values[insertPos],
                (vb->cardinality - (uint32_t)insertPos) * sizeof(uint16_t));

        vb->container.array.values[insertPos] = value;
        vb->cardinality++;
        return true;
    }

    case VARINT_BITMAP_BITMAP: {
        if (bitmapSet_(vb->container.bitmap.bits, value)) {
            vb->cardinality++;
            return true;
        }
        return false;
    }

    case VARINT_BITMAP_RUNS:
        /* For simplicity, convert to array or bitmap */
        if (vb->cardinality >= VARINT_BITMAP_ARRAY_MAX) {
            /* Convert runs to bitmap */
            uint8_t *bits = calloc(VARINT_BITMAP_BITMAP_SIZE, 1);
            if (!bits) {
                return false; /* Out of memory */
            }

            for (uint32_t i = 0; i < vb->container.runs.numRuns; i++) {
                uint16_t start = vb->container.runs.runs[i * 2];
                uint16_t length = vb->container.runs.runs[i * 2 + 1];
                for (uint16_t j = 0; j < length; j++) {
                    bitmapSet_(bits, start + j);
                }
            }

            free(vb->container.runs.runs);
            vb->type = VARINT_BITMAP_BITMAP;
            vb->container.bitmap.bits = bits;

            return varintBitmapAdd(vb, value);
        } else {
            /* Convert runs to array */
            uint16_t *values = malloc((vb->cardinality + 1) * sizeof(uint16_t));
            if (!values) {
                return false; /* Out of memory */
            }

            uint32_t pos = 0;
            for (uint32_t i = 0; i < vb->container.runs.numRuns; i++) {
                uint16_t start = vb->container.runs.runs[i * 2];
                uint16_t length = vb->container.runs.runs[i * 2 + 1];
                for (uint16_t j = 0; j < length; j++) {
                    values[pos++] = start + j;
                }
            }

            free(vb->container.runs.runs);
            vb->type = VARINT_BITMAP_ARRAY;
            vb->container.array.values = values;
            vb->container.array.capacity = vb->cardinality + 1;

            return varintBitmapAdd(vb, value);
        }
    }

    return false;
}

bool varintBitmapRemove(varintBitmap *vb, uint16_t value) {
    switch (vb->type) {
    case VARINT_BITMAP_ARRAY: {
        int32_t idx =
            binarySearch_(vb->container.array.values, vb->cardinality, value);
        if (idx < 0) {
            return false; /* Not present */
        }

        /* Shift elements left */
        memmove(&vb->container.array.values[idx],
                &vb->container.array.values[idx + 1],
                (vb->cardinality - (uint32_t)idx - 1) * sizeof(uint16_t));

        vb->cardinality--;
        return true;
    }

    case VARINT_BITMAP_BITMAP: {
        if (bitmapClear_(vb->container.bitmap.bits, value)) {
            vb->cardinality--;

            /* Convert to array if too sparse */
            if (vb->cardinality < VARINT_BITMAP_ARRAY_MAX) {
                if (!bitmapToArray_(vb)) {
                    /* Failed to convert, but value was removed from bitmap */
                    return true; /* Still report success */
                }
            }

            return true;
        }
        return false;
    }

    case VARINT_BITMAP_RUNS:
        /* For simplicity, convert first */
        if (vb->cardinality >= VARINT_BITMAP_ARRAY_MAX) {
            uint8_t *bits = calloc(VARINT_BITMAP_BITMAP_SIZE, 1);
            if (!bits) {
                return false; /* Out of memory */
            }

            for (uint32_t i = 0; i < vb->container.runs.numRuns; i++) {
                uint16_t start = vb->container.runs.runs[i * 2];
                uint16_t length = vb->container.runs.runs[i * 2 + 1];
                for (uint16_t j = 0; j < length; j++) {
                    bitmapSet_(bits, start + j);
                }
            }

            free(vb->container.runs.runs);
            vb->type = VARINT_BITMAP_BITMAP;
            vb->container.bitmap.bits = bits;
        } else {
            uint16_t *values = malloc(vb->cardinality * sizeof(uint16_t));
            if (!values) {
                return false; /* Out of memory */
            }

            uint32_t pos = 0;
            for (uint32_t i = 0; i < vb->container.runs.numRuns; i++) {
                uint16_t start = vb->container.runs.runs[i * 2];
                uint16_t length = vb->container.runs.runs[i * 2 + 1];
                for (uint16_t j = 0; j < length; j++) {
                    values[pos++] = start + j;
                }
            }

            free(vb->container.runs.runs);
            vb->type = VARINT_BITMAP_ARRAY;
            vb->container.array.values = values;
            vb->container.array.capacity = vb->cardinality;
        }

        return varintBitmapRemove(vb, value);
    }

    return false;
}

bool varintBitmapContains(const varintBitmap *vb, uint16_t value) {
    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        return binarySearch_(vb->container.array.values, vb->cardinality,
                             value) >= 0;

    case VARINT_BITMAP_BITMAP:
        return bitmapContains_(vb->container.bitmap.bits, value);

    case VARINT_BITMAP_RUNS:
        for (uint32_t i = 0; i < vb->container.runs.numRuns; i++) {
            uint16_t start = vb->container.runs.runs[i * 2];
            uint16_t length = vb->container.runs.runs[i * 2 + 1];
            if (value >= start && value < start + length) {
                return true;
            }
            if (value < start) {
                return false; /* Runs are sorted */
            }
        }
        return false;
    }

    return false;
}

varintBitmap *varintBitmapAnd(const varintBitmap *vb1,
                              const varintBitmap *vb2) {
    varintBitmap *result = varintBitmapCreate();

    /* Optimize: AND with array containers */
    if (vb1->type == VARINT_BITMAP_ARRAY && vb2->type == VARINT_BITMAP_ARRAY) {
        /* Intersect two sorted arrays */
        uint32_t i = 0, j = 0;
        while (i < vb1->cardinality && j < vb2->cardinality) {
            uint16_t v1 = vb1->container.array.values[i];
            uint16_t v2 = vb2->container.array.values[j];

            if (v1 == v2) {
                varintBitmapAdd(result, v1);
                i++;
                j++;
            } else if (v1 < v2) {
                i++;
            } else {
                j++;
            }
        }
        return result;
    }

    /* General case: iterate smaller set and check membership */
    const varintBitmap *smaller =
        vb1->cardinality < vb2->cardinality ? vb1 : vb2;
    const varintBitmap *other = vb1->cardinality < vb2->cardinality ? vb2 : vb1;

    varintBitmapIterator it = varintBitmapCreateIterator(smaller);
    while (varintBitmapIteratorNext(&it)) {
        if (varintBitmapContains(other, it.currentValue)) {
            varintBitmapAdd(result, it.currentValue);
        }
    }

    return result;
}

varintBitmap *varintBitmapOr(const varintBitmap *vb1, const varintBitmap *vb2) {
    varintBitmap *result = varintBitmapClone(vb1);

    varintBitmapIterator it = varintBitmapCreateIterator(vb2);
    while (varintBitmapIteratorNext(&it)) {
        varintBitmapAdd(result, it.currentValue);
    }

    return result;
}

varintBitmap *varintBitmapXor(const varintBitmap *vb1,
                              const varintBitmap *vb2) {
    varintBitmap *result = varintBitmapCreate();

    /* Add elements from vb1 that are not in vb2 */
    varintBitmapIterator it1 = varintBitmapCreateIterator(vb1);
    while (varintBitmapIteratorNext(&it1)) {
        if (!varintBitmapContains(vb2, it1.currentValue)) {
            varintBitmapAdd(result, it1.currentValue);
        }
    }

    /* Add elements from vb2 that are not in vb1 */
    varintBitmapIterator it2 = varintBitmapCreateIterator(vb2);
    while (varintBitmapIteratorNext(&it2)) {
        if (!varintBitmapContains(vb1, it2.currentValue)) {
            varintBitmapAdd(result, it2.currentValue);
        }
    }

    return result;
}

varintBitmap *varintBitmapAndNot(const varintBitmap *vb1,
                                 const varintBitmap *vb2) {
    varintBitmap *result = varintBitmapCreate();

    varintBitmapIterator it = varintBitmapCreateIterator(vb1);
    while (varintBitmapIteratorNext(&it)) {
        if (!varintBitmapContains(vb2, it.currentValue)) {
            varintBitmapAdd(result, it.currentValue);
        }
    }

    return result;
}

uint32_t varintBitmapCardinality(const varintBitmap *vb) {
    return vb->cardinality;
}

size_t varintBitmapSizeBytes(const varintBitmap *vb) {
    size_t size = sizeof(varintBitmap);

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        size += vb->container.array.capacity * sizeof(uint16_t);
        break;
    case VARINT_BITMAP_BITMAP:
        size += VARINT_BITMAP_BITMAP_SIZE;
        break;
    case VARINT_BITMAP_RUNS:
        size += vb->container.runs.capacity * 2 * sizeof(uint16_t);
        break;
    }

    return size;
}

size_t varintBitmapEncode(const varintBitmap *vb, uint8_t *buffer) {
    uint8_t *start = buffer;

    /* Write type */
    *buffer++ = (uint8_t)vb->type;

    /* Write cardinality */
    memcpy(buffer, &vb->cardinality, sizeof(uint32_t));
    buffer += sizeof(uint32_t);

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        /* Write array values */
        memcpy(buffer, vb->container.array.values,
               vb->cardinality * sizeof(uint16_t));
        buffer += vb->cardinality * sizeof(uint16_t);
        break;

    case VARINT_BITMAP_BITMAP:
        /* Write bitmap bits */
        memcpy(buffer, vb->container.bitmap.bits, VARINT_BITMAP_BITMAP_SIZE);
        buffer += VARINT_BITMAP_BITMAP_SIZE;
        break;

    case VARINT_BITMAP_RUNS:
        /* Write number of runs */
        memcpy(buffer, &vb->container.runs.numRuns, sizeof(uint32_t));
        buffer += sizeof(uint32_t);
        /* Write run pairs */
        memcpy(buffer, vb->container.runs.runs,
               vb->container.runs.numRuns * 2 * sizeof(uint16_t));
        buffer += vb->container.runs.numRuns * 2 * sizeof(uint16_t);
        break;
    }

    return (size_t)(buffer - start);
}

varintBitmap *varintBitmapDecode(const uint8_t *buffer, size_t len) {
    (void)len; /* Unused, but kept for API consistency */

    varintBitmap *vb = malloc(sizeof(varintBitmap));
    if (!vb) {
        return NULL; /* Out of memory */
    }

    /* Read type */
    vb->type = (varintBitmapContainerType)*buffer++;

    /* Read cardinality */
    memcpy(&vb->cardinality, buffer, sizeof(uint32_t));
    buffer += sizeof(uint32_t);

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        vb->container.array.capacity = vb->cardinality;
        vb->container.array.values = malloc(vb->cardinality * sizeof(uint16_t));
        if (!vb->container.array.values) {
            free(vb);
            return NULL; /* Out of memory */
        }
        memcpy(vb->container.array.values, buffer,
               vb->cardinality * sizeof(uint16_t));
        break;

    case VARINT_BITMAP_BITMAP:
        vb->container.bitmap.bits = malloc(VARINT_BITMAP_BITMAP_SIZE);
        if (!vb->container.bitmap.bits) {
            free(vb);
            return NULL; /* Out of memory */
        }
        memcpy(vb->container.bitmap.bits, buffer, VARINT_BITMAP_BITMAP_SIZE);
        break;

    case VARINT_BITMAP_RUNS:
        memcpy(&vb->container.runs.numRuns, buffer, sizeof(uint32_t));
        buffer += sizeof(uint32_t);
        vb->container.runs.capacity = vb->container.runs.numRuns;
        vb->container.runs.runs =
            malloc(vb->container.runs.numRuns * 2 * sizeof(uint16_t));
        if (!vb->container.runs.runs) {
            free(vb);
            return NULL; /* Out of memory */
        }
        memcpy(vb->container.runs.runs, buffer,
               vb->container.runs.numRuns * 2 * sizeof(uint16_t));
        break;
    }

    return vb;
}

varintBitmapIterator varintBitmapCreateIterator(const varintBitmap *vb) {
    varintBitmapIterator it;
    it.vb = vb;
    it.position = 0;
    it.currentValue = 0;
    it.hasValue = false;
    return it;
}

bool varintBitmapIteratorNext(varintBitmapIterator *it) {
    switch (it->vb->type) {
    case VARINT_BITMAP_ARRAY:
        if (it->position < it->vb->cardinality) {
            it->currentValue = it->vb->container.array.values[it->position];
            it->position++;
            it->hasValue = true;
            return true;
        }
        break;

    case VARINT_BITMAP_BITMAP:
        /* Find next set bit */
        while (it->position < VARINT_BITMAP_MAX_VALUE) {
            if (bitmapContains_(it->vb->container.bitmap.bits,
                                (uint16_t)it->position)) {
                it->currentValue = (uint16_t)it->position;
                it->position++;
                it->hasValue = true;
                return true;
            }
            it->position++;
        }
        break;

    case VARINT_BITMAP_RUNS: {
        /* Iterate through runs */
        uint32_t runIdx = it->position / 65536;
        uint16_t offsetInRun = (uint16_t)(it->position % 65536U);

        if (runIdx >= it->vb->container.runs.numRuns) {
            break;
        }

        uint16_t start = it->vb->container.runs.runs[runIdx * 2];
        uint16_t length = it->vb->container.runs.runs[runIdx * 2 + 1];

        if (offsetInRun < length) {
            it->currentValue = start + offsetInRun;
            it->position++;
            it->hasValue = true;
            return true;
        }

        /* Move to next run */
        it->position = (runIdx + 1) * 65536;
        return varintBitmapIteratorNext(it);
    }
    }

    it->hasValue = false;
    return false;
}

void varintBitmapAddMany(varintBitmap *vb, const uint16_t *values,
                         uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        varintBitmapAdd(vb, values[i]);
    }
}

uint32_t varintBitmapToArray(const varintBitmap *vb, uint16_t *output) {
    uint32_t count = 0;
    varintBitmapIterator it = varintBitmapCreateIterator(vb);
    while (varintBitmapIteratorNext(&it)) {
        output[count++] = it.currentValue;
    }
    return count;
}

void varintBitmapGetStats(const varintBitmap *vb, varintBitmapStats *stats) {
    stats->type = vb->type;
    stats->cardinality = vb->cardinality;
    stats->sizeBytes = varintBitmapSizeBytes(vb);

    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        stats->containerCapacity = vb->container.array.capacity;
        break;
    case VARINT_BITMAP_BITMAP:
        stats->containerCapacity = VARINT_BITMAP_BITMAP_SIZE * 8;
        break;
    case VARINT_BITMAP_RUNS:
        stats->containerCapacity = vb->container.runs.capacity;
        break;
    }
}

void varintBitmapOptimize(varintBitmap *vb) {
    /* Already optimized by automatic conversions */
    (void)vb;
}

bool varintBitmapIsEmpty(const varintBitmap *vb) {
    return vb->cardinality == 0;
}

void varintBitmapClear(varintBitmap *vb) {
    vb->cardinality = 0;

    /* Keep current container type, just clear data */
    switch (vb->type) {
    case VARINT_BITMAP_ARRAY:
        /* Already cleared by cardinality = 0 */
        break;
    case VARINT_BITMAP_BITMAP:
        memset(vb->container.bitmap.bits, 0, VARINT_BITMAP_BITMAP_SIZE);
        break;
    case VARINT_BITMAP_RUNS:
        vb->container.runs.numRuns = 0;
        break;
    }
}

void varintBitmapAddRange(varintBitmap *vb, uint16_t min, uint16_t max) {
    if (min >= max) {
        return;
    }

    uint32_t rangeSize = max - min;

    /* For large ranges, use runs container */
    if (rangeSize > VARINT_BITMAP_ARRAY_MAX) {
        /* Convert to runs if beneficial */
        if (vb->type == VARINT_BITMAP_ARRAY) {
            free(vb->container.array.values);
        } else if (vb->type == VARINT_BITMAP_BITMAP) {
            free(vb->container.bitmap.bits);
        }

        vb->type = VARINT_BITMAP_RUNS;
        vb->container.runs.numRuns = 1;
        vb->container.runs.capacity = 1;
        vb->container.runs.runs = malloc(2 * sizeof(uint16_t));
        if (!vb->container.runs.runs) {
            /* Out of memory - reset to empty array container */
            vb->type = VARINT_BITMAP_ARRAY;
            vb->cardinality = 0;
            vb->container.array.values = NULL;
            vb->container.array.capacity = 0;
            return;
        }
        vb->container.runs.runs[0] = min;
        vb->container.runs.runs[1] = (uint16_t)rangeSize;
        vb->cardinality = rangeSize;
        return;
    }

    /* Otherwise add individually */
    for (uint16_t i = min; i < max; i++) {
        varintBitmapAdd(vb, i);
    }
}

void varintBitmapRemoveRange(varintBitmap *vb, uint16_t min, uint16_t max) {
    for (uint16_t i = min; i < max; i++) {
        varintBitmapRemove(vb, i);
    }
}
