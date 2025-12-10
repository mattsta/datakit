#include "intsetSmall.h"
#include "datakit.h"
#include <stdlib.h>
#include <string.h>

/* Create new empty small intset */
intsetSmall *intsetSmallNew(void) {
    intsetSmall *is = zcalloc(1, sizeof(intsetSmall));
    is->count16 = 0;
    return is;
}

/* Create small intset from sorted int16 array */
intsetSmall *intsetSmallFromArray(const int16_t *values, uint32_t count) {
    if (!values || count == 0) {
        return intsetSmallNew();
    }

    intsetSmall *is = zcalloc(1, sizeof(intsetSmall) + count * sizeof(int16_t));
    is->count16 = count;
    memcpy(is->values16, values, count * sizeof(int16_t));
    return is;
}

/* Free small intset */
void intsetSmallFree(intsetSmall *is) {
    if (is) {
        zfree(is);
    }
}

/* Binary search for value in int16 array
 * Returns INTSET_FOUND if found, INTSET_NOT_FOUND otherwise
 * Sets *pos to found position or insert position */
intsetSearchResult intsetSmallFind(const intsetSmall *is, int64_t value,
                                   uint32_t *pos) {
    if (!is || is->count16 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    /* Value must fit in int16_t to be in this tier */
    if (!intsetValueFitsInt16(value)) {
        /* Value is too large/small for this tier */
        if (value < is->values16[0]) {
            if (pos) {
                *pos = 0;
            }
        } else {
            if (pos) {
                *pos = is->count16;
            }
        }
        return INTSET_NOT_FOUND;
    }

    int16_t val16 = (int16_t)value;
    uint32_t left = 0;
    uint32_t right = is->count16;

    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        int16_t midval = is->values16[mid];

        if (midval < val16) {
            left = mid + 1;
        } else if (midval > val16) {
            right = mid;
        } else {
            if (pos) {
                *pos = mid;
            }
            return INTSET_FOUND;
        }
    }

    if (pos) {
        *pos = left;
    }
    return INTSET_NOT_FOUND;
}

/* Get value at position */
bool intsetSmallGet(const intsetSmall *is, uint32_t pos, int64_t *value) {
    if (!is || pos >= is->count16) {
        return false;
    }

    if (value) {
        *value = is->values16[pos];
    }
    return true;
}

/* Add value to small intset */
intsetSmall *intsetSmallAdd(intsetSmall *is, int64_t value, bool *added) {
    if (!is) {
        is = intsetSmallNew();
    }

    /* Value must fit in int16_t for small tier */
    if (!intsetValueFitsInt16(value)) {
        if (added) {
            *added = false;
        }
        return is;
    }

    uint32_t pos;
    intsetSearchResult result = intsetSmallFind(is, value, &pos);

    if (result == INTSET_FOUND) {
        /* Already exists */
        if (added) {
            *added = false;
        }
        return is;
    }

    /* Need to insert at pos */
    uint32_t newCount = is->count16 + 1;
    size_t newSize = sizeof(intsetSmall) + newCount * sizeof(int16_t);
    intsetSmall *newIs = zrealloc(is, newSize);

    /* Shift elements to make room */
    if (pos < newIs->count16) {
        memmove(&newIs->values16[pos + 1], &newIs->values16[pos],
                (newIs->count16 - pos) * sizeof(int16_t));
    }

    /* Insert new value */
    newIs->values16[pos] = (int16_t)value;
    newIs->count16 = newCount;

    if (added) {
        *added = true;
    }
    return newIs;
}

/* Remove value from small intset */
intsetSmall *intsetSmallRemove(intsetSmall *is, int64_t value, bool *removed) {
    if (!is) {
        if (removed) {
            *removed = false;
        }
        return NULL;
    }

    uint32_t pos;
    intsetSearchResult result = intsetSmallFind(is, value, &pos);

    if (result == INTSET_NOT_FOUND) {
        if (removed) {
            *removed = false;
        }
        return is;
    }

    /* Remove element at pos */
    if (pos < is->count16 - 1) {
        memmove(&is->values16[pos], &is->values16[pos + 1],
                (is->count16 - pos - 1) * sizeof(int16_t));
    }

    is->count16--;

    /* Optionally shrink allocation (following platform pattern of not
     * shrinking) */
    /* For now, keep allocation same size */

    if (removed) {
        *removed = true;
    }
    return is;
}

/* Check if should upgrade to medium tier */
bool intsetSmallShouldUpgrade(const intsetSmall *is, int64_t nextValue) {
    if (!is) {
        /* If adding first value and it doesn't fit int16, start with medium */
        return !intsetValueFitsInt16(nextValue);
    }

    /* Upgrade if:
     * 1. Value doesn't fit in int16_t
     * 2. Would exceed max count
     * 3. Would exceed max bytes */

    if (!intsetValueFitsInt16(nextValue)) {
        return true;
    }

    if (is->count16 >= INTSET_SMALL_MAX_COUNT) {
        return true;
    }

    size_t currentBytes = intsetSmallBytes(is);
    if (currentBytes >= INTSET_SMALL_MAX_BYTES) {
        return true;
    }

    return false;
}

/* Initialize iterator */
void intsetSmallIteratorInit(intsetSmallIterator *it, const intsetSmall *is) {
    it->is = is;
    it->pos = 0;
}

/* Get next value from iterator */
bool intsetSmallIteratorNext(intsetSmallIterator *it, int64_t *value) {
    if (!it || !it->is || it->pos >= it->is->count16) {
        return false;
    }

    if (value) {
        *value = it->is->values16[it->pos];
    }
    it->pos++;
    return true;
}
