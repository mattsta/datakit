#include "intsetMedium.h"
#include "datakit.h"
#include <stdlib.h>
#include <string.h>

/* Create new empty medium intset */
intsetMedium *intsetMediumNew(void) {
    intsetMedium *m = zcalloc(1, sizeof(intsetMedium));
    m->count16 = 0;
    m->count32 = 0;
    m->values16 = NULL;
    m->values32 = NULL;
    return m;
}

/* Create medium intset by upgrading from small */
intsetMedium *intsetMediumFromSmall(const intsetSmall *small) {
    intsetMedium *m = intsetMediumNew();

    if (!small || small->count16 == 0) {
        return m;
    }

    /* Copy all int16 values */
    m->count16 = small->count16;
    m->values16 = zcalloc(m->count16, sizeof(int16_t));
    memcpy(m->values16, small->values16, m->count16 * sizeof(int16_t));

    return m;
}

/* Free medium intset */
void intsetMediumFree(intsetMedium *m) {
    if (m) {
        if (m->values16) {
            zfree(m->values16);
        }
        if (m->values32) {
            zfree(m->values32);
        }
        zfree(m);
    }
}

/* Get total bytes */
size_t intsetMediumBytes(const intsetMedium *m) {
    if (!m) {
        return 0;
    }

    size_t bytes = sizeof(intsetMedium);
    bytes += m->count16 * sizeof(int16_t);
    bytes += m->count32 * sizeof(int32_t);
    return bytes;
}

/* Binary search in int16 array */
static intsetSearchResult intsetMediumFind16(const intsetMedium *m,
                                             int16_t val16, uint32_t *pos) {
    if (!m || m->count16 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    uint32_t left = 0;
    uint32_t right = m->count16;

    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        int16_t midval = m->values16[mid];

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

/* Binary search in int32 array */
static intsetSearchResult intsetMediumFind32(const intsetMedium *m,
                                             int32_t val32, uint32_t *pos) {
    if (!m || m->count32 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    uint32_t left = 0;
    uint32_t right = m->count32;

    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        int32_t midval = m->values32[mid];

        if (midval < val32) {
            left = mid + 1;
        } else if (midval > val32) {
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

/* Find value in medium intset */
intsetSearchResult intsetMediumFind(const intsetMedium *m, int64_t value,
                                    uint64_t *pos) {
    if (!m) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    /* Determine which array to search */
    if (intsetValueFitsInt16(value)) {
        /* Search int16 array */
        uint32_t pos16;
        intsetSearchResult result =
            intsetMediumFind16(m, (int16_t)value, &pos16);
        if (pos) {
            *pos = pos16;
        }
        return result;
    } else if (intsetValueFitsInt32(value)) {
        /* Search int32 array */
        uint32_t pos32;
        intsetSearchResult result =
            intsetMediumFind32(m, (int32_t)value, &pos32);
        /* Position in merged view is after all int16 values */
        if (pos) {
            *pos = (uint64_t)m->count16 + pos32;
        }
        return result;
    } else {
        /* Value doesn't fit in int32, not in this tier */
        if (pos) {
            *pos = (uint64_t)m->count16 + m->count32;
        }
        return INTSET_NOT_FOUND;
    }
}

/* Get value at position in merged view (virtual merge of two sorted arrays)
 * This is O(pos) but provides correct sorted order semantics */
bool intsetMediumGet(const intsetMedium *m, uint64_t pos, int64_t *value) {
    if (!m) {
        return false;
    }

    uint64_t totalCount = (uint64_t)m->count16 + (uint64_t)m->count32;
    if (pos >= totalCount) {
        return false;
    }

    /* Virtual merge of two sorted arrays using two pointers */
    uint32_t i16 = 0, i32 = 0;

    for (uint64_t i = 0; i <= pos; i++) {
        /* Find minimum of current positions in each array */
        bool has16 = (i16 < m->count16);
        bool has32 = (i32 < m->count32);

        int64_t v16 = has16 ? m->values16[i16] : INT64_MAX;
        int64_t v32 = has32 ? m->values32[i32] : INT64_MAX;

        int64_t minVal;
        if (has16 && v16 <= v32) {
            minVal = v16;
            i16++;
        } else {
            minVal = v32;
            i32++;
        }

        if (i == pos) {
            if (value) {
                *value = minVal;
            }
            return true;
        }
    }

    return false;
}

/* Add value to medium intset */
intsetMedium *intsetMediumAdd(intsetMedium *m, int64_t value, bool *added) {
    if (!m) {
        m = intsetMediumNew();
    }

    /* Value must fit in int32_t for medium tier */
    if (!intsetValueFitsInt32(value)) {
        if (added) {
            *added = false;
        }
        return m;
    }

    if (intsetValueFitsInt16(value)) {
        /* Add to int16 array */
        uint32_t pos16;
        intsetSearchResult result =
            intsetMediumFind16(m, (int16_t)value, &pos16);

        if (result == INTSET_FOUND) {
            if (added) {
                *added = false;
            }
            return m;
        }

        /* Insert at pos16 */
        uint32_t newCount = m->count16 + 1;
        int16_t *newValues = zcalloc(newCount, sizeof(int16_t));

        /* Copy before insert point */
        if (pos16 > 0) {
            memcpy(newValues, m->values16, pos16 * sizeof(int16_t));
        }

        /* Insert new value */
        newValues[pos16] = (int16_t)value;

        /* Copy after insert point */
        if (pos16 < m->count16) {
            memcpy(&newValues[pos16 + 1], &m->values16[pos16],
                   (m->count16 - pos16) * sizeof(int16_t));
        }

        if (m->values16) {
            zfree(m->values16);
        }
        m->values16 = newValues;
        m->count16 = newCount;

    } else {
        /* Add to int32 array */
        uint32_t pos32;
        intsetSearchResult result =
            intsetMediumFind32(m, (int32_t)value, &pos32);

        if (result == INTSET_FOUND) {
            if (added) {
                *added = false;
            }
            return m;
        }

        /* Insert at pos32 */
        uint32_t newCount = m->count32 + 1;
        int32_t *newValues = zcalloc(newCount, sizeof(int32_t));

        /* Copy before insert point */
        if (pos32 > 0) {
            memcpy(newValues, m->values32, pos32 * sizeof(int32_t));
        }

        /* Insert new value */
        newValues[pos32] = (int32_t)value;

        /* Copy after insert point */
        if (pos32 < m->count32) {
            memcpy(&newValues[pos32 + 1], &m->values32[pos32],
                   (m->count32 - pos32) * sizeof(int32_t));
        }

        if (m->values32) {
            zfree(m->values32);
        }
        m->values32 = newValues;
        m->count32 = newCount;
    }

    if (added) {
        *added = true;
    }
    return m;
}

/* Remove value from medium intset */
intsetMedium *intsetMediumRemove(intsetMedium *m, int64_t value,
                                 bool *removed) {
    if (!m) {
        if (removed) {
            *removed = false;
        }
        return NULL;
    }

    if (intsetValueFitsInt16(value)) {
        /* Remove from int16 array */
        uint32_t pos16;
        intsetSearchResult result =
            intsetMediumFind16(m, (int16_t)value, &pos16);

        if (result == INTSET_NOT_FOUND) {
            if (removed) {
                *removed = false;
            }
            return m;
        }

        /* Remove at pos16 */
        if (m->count16 == 1) {
            /* Last element */
            zfree(m->values16);
            m->values16 = NULL;
            m->count16 = 0;
        } else {
            uint32_t newCount = m->count16 - 1;
            int16_t *newValues = zcalloc(newCount, sizeof(int16_t));

            /* Copy before removed element */
            if (pos16 > 0) {
                memcpy(newValues, m->values16, pos16 * sizeof(int16_t));
            }

            /* Copy after removed element */
            if (pos16 < m->count16 - 1) {
                memcpy(&newValues[pos16], &m->values16[pos16 + 1],
                       (m->count16 - pos16 - 1) * sizeof(int16_t));
            }

            zfree(m->values16);
            m->values16 = newValues;
            m->count16 = newCount;
        }

    } else if (intsetValueFitsInt32(value)) {
        /* Remove from int32 array */
        uint32_t pos32;
        intsetSearchResult result =
            intsetMediumFind32(m, (int32_t)value, &pos32);

        if (result == INTSET_NOT_FOUND) {
            if (removed) {
                *removed = false;
            }
            return m;
        }

        /* Remove at pos32 */
        if (m->count32 == 1) {
            /* Last element */
            zfree(m->values32);
            m->values32 = NULL;
            m->count32 = 0;
        } else {
            uint32_t newCount = m->count32 - 1;
            int32_t *newValues = zcalloc(newCount, sizeof(int32_t));

            /* Copy before removed element */
            if (pos32 > 0) {
                memcpy(newValues, m->values32, pos32 * sizeof(int32_t));
            }

            /* Copy after removed element */
            if (pos32 < m->count32 - 1) {
                memcpy(&newValues[pos32], &m->values32[pos32 + 1],
                       (m->count32 - pos32 - 1) * sizeof(int32_t));
            }

            zfree(m->values32);
            m->values32 = newValues;
            m->count32 = newCount;
        }

    } else {
        if (removed) {
            *removed = false;
        }
        return m;
    }

    if (removed) {
        *removed = true;
    }
    return m;
}

/* Check if should upgrade to full tier */
bool intsetMediumShouldUpgrade(const intsetMedium *m, int64_t nextValue) {
    if (!m) {
        /* If adding first value and it doesn't fit int32, start with full */
        return !intsetValueFitsInt32(nextValue);
    }

    /* Upgrade if:
     * 1. Value doesn't fit in int32_t
     * 2. Would exceed max total count
     * 3. Would exceed max bytes */

    if (!intsetValueFitsInt32(nextValue)) {
        return true;
    }

    uint64_t totalCount = intsetMediumCount(m);
    if (totalCount >= INTSET_MEDIUM_MAX_COUNT) {
        return true;
    }

    size_t currentBytes = intsetMediumBytes(m);
    if (currentBytes >= INTSET_MEDIUM_MAX_BYTES) {
        return true;
    }

    return false;
}

/* Initialize iterator */
void intsetMediumIteratorInit(intsetMediumIterator *it, const intsetMedium *m) {
    it->m = m;
    it->pos16 = 0;
    it->pos32 = 0;
}

/* Get next value from iterator - performs virtual merge */
bool intsetMediumIteratorNext(intsetMediumIterator *it, int64_t *value) {
    if (!it || !it->m) {
        return false;
    }

    /* Virtual merge of two sorted arrays */
    bool has16 = it->pos16 < it->m->count16;
    bool has32 = it->pos32 < it->m->count32;

    if (!has16 && !has32) {
        return false;
    }

    if (!has32 ||
        (has16 && it->m->values16[it->pos16] < it->m->values32[it->pos32])) {
        /* Take from int16 array */
        if (value) {
            *value = it->m->values16[it->pos16];
        }
        it->pos16++;
    } else {
        /* Take from int32 array */
        if (value) {
            *value = it->m->values32[it->pos32];
        }
        it->pos32++;
    }

    return true;
}
