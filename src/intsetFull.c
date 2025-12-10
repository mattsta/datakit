#include "intsetFull.h"
#include "datakit.h"
#include <stdlib.h>
#include <string.h>

/* Create new empty full intset */
intsetFull *intsetFullNew(void) {
    intsetFull *f = zcalloc(1, sizeof(intsetFull));
    f->count16 = 0;
    f->count32 = 0;
    f->count64 = 0;
    f->values16 = NULL;
    f->values32 = NULL;
    f->values64 = NULL;
    return f;
}

/* Create full intset by upgrading from medium */
intsetFull *intsetFullFromMedium(const intsetMedium *medium) {
    intsetFull *f = intsetFullNew();

    if (!medium) {
        return f;
    }

    /* Copy all int16 values */
    if (medium->count16 > 0) {
        f->count16 = medium->count16;
        f->values16 = zcalloc(f->count16, sizeof(int16_t));
        memcpy(f->values16, medium->values16, f->count16 * sizeof(int16_t));
    }

    /* Copy all int32 values */
    if (medium->count32 > 0) {
        f->count32 = medium->count32;
        f->values32 = zcalloc(f->count32, sizeof(int32_t));
        memcpy(f->values32, medium->values32, f->count32 * sizeof(int32_t));
    }

    return f;
}

/* Free full intset */
void intsetFullFree(intsetFull *f) {
    if (f) {
        if (f->values16) {
            zfree(f->values16);
        }
        if (f->values32) {
            zfree(f->values32);
        }
        if (f->values64) {
            zfree(f->values64);
        }
        zfree(f);
    }
}

/* Get total bytes */
size_t intsetFullBytes(const intsetFull *f) {
    if (!f) {
        return 0;
    }

    size_t bytes = sizeof(intsetFull);
    bytes += f->count16 * sizeof(int16_t);
    bytes += f->count32 * sizeof(int32_t);
    bytes += f->count64 * sizeof(int64_t);
    return bytes;
}

/* Binary search in int16 array */
static intsetSearchResult intsetFullFind16(const intsetFull *f, int16_t val16,
                                           uint64_t *pos) {
    if (!f || f->count16 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    uint64_t left = 0;
    uint64_t right = f->count16;

    while (left < right) {
        uint64_t mid = left + (right - left) / 2;
        int16_t midval = f->values16[mid];

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
static intsetSearchResult intsetFullFind32(const intsetFull *f, int32_t val32,
                                           uint64_t *pos) {
    if (!f || f->count32 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    uint64_t left = 0;
    uint64_t right = f->count32;

    while (left < right) {
        uint64_t mid = left + (right - left) / 2;
        int32_t midval = f->values32[mid];

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

/* Binary search in int64 array */
static intsetSearchResult intsetFullFind64(const intsetFull *f, int64_t val64,
                                           uint64_t *pos) {
    if (!f || f->count64 == 0) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    uint64_t left = 0;
    uint64_t right = f->count64;

    while (left < right) {
        uint64_t mid = left + (right - left) / 2;
        int64_t midval = f->values64[mid];

        if (midval < val64) {
            left = mid + 1;
        } else if (midval > val64) {
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

/* Find value in full intset */
intsetSearchResult intsetFullFind(const intsetFull *f, int64_t value,
                                  uint64_t *pos) {
    if (!f) {
        if (pos) {
            *pos = 0;
        }
        return INTSET_NOT_FOUND;
    }

    /* Determine which array to search */
    if (intsetValueFitsInt16(value)) {
        /* Search int16 array */
        uint64_t pos16;
        intsetSearchResult result = intsetFullFind16(f, (int16_t)value, &pos16);
        if (pos) {
            *pos = pos16;
        }
        return result;
    } else if (intsetValueFitsInt32(value)) {
        /* Search int32 array */
        uint64_t pos32;
        intsetSearchResult result = intsetFullFind32(f, (int32_t)value, &pos32);
        /* Position in merged view is after all int16 values */
        if (pos) {
            *pos = f->count16 + pos32;
        }
        return result;
    } else {
        /* Search int64 array */
        uint64_t pos64;
        intsetSearchResult result = intsetFullFind64(f, value, &pos64);
        /* Position in merged view is after all int16 and int32 values */
        if (pos) {
            *pos = f->count16 + f->count32 + pos64;
        }
        return result;
    }
}

/* Get value at position in merged view (virtual merge of three sorted arrays)
 * This is O(pos) but provides correct sorted order semantics */
bool intsetFullGet(const intsetFull *f, uint64_t pos, int64_t *value) {
    if (!f) {
        return false;
    }

    uint64_t totalCount = f->count16 + f->count32 + f->count64;
    if (pos >= totalCount) {
        return false;
    }

    /* Virtual merge of three sorted arrays using three pointers */
    uint64_t i16 = 0, i32 = 0, i64 = 0;

    for (uint64_t i = 0; i <= pos; i++) {
        /* Find minimum of current positions in each array */
        bool has16 = (i16 < f->count16);
        bool has32 = (i32 < f->count32);
        bool has64 = (i64 < f->count64);

        int64_t v16 = has16 ? f->values16[i16] : INT64_MAX;
        int64_t v32 = has32 ? f->values32[i32] : INT64_MAX;
        int64_t v64 = has64 ? f->values64[i64] : INT64_MAX;

        int64_t minVal;
        if (has16 && v16 <= v32 && v16 <= v64) {
            minVal = v16;
            i16++;
        } else if (has32 && v32 <= v64) {
            minVal = v32;
            i32++;
        } else {
            minVal = v64;
            i64++;
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

/* Add value to full intset */
intsetFull *intsetFullAdd(intsetFull *f, int64_t value, bool *added) {
    if (!f) {
        f = intsetFullNew();
    }

    if (intsetValueFitsInt16(value)) {
        /* Add to int16 array */
        uint64_t pos16;
        intsetSearchResult result = intsetFullFind16(f, (int16_t)value, &pos16);

        if (result == INTSET_FOUND) {
            if (added) {
                *added = false;
            }
            return f;
        }

        /* Insert at pos16 */
        uint64_t newCount = f->count16 + 1;
        int16_t *newValues = zcalloc(newCount, sizeof(int16_t));

        /* Copy before insert point */
        if (pos16 > 0) {
            memcpy(newValues, f->values16, pos16 * sizeof(int16_t));
        }

        /* Insert new value */
        newValues[pos16] = (int16_t)value;

        /* Copy after insert point */
        if (pos16 < f->count16) {
            memcpy(&newValues[pos16 + 1], &f->values16[pos16],
                   (f->count16 - pos16) * sizeof(int16_t));
        }

        if (f->values16) {
            zfree(f->values16);
        }
        f->values16 = newValues;
        f->count16 = newCount;

    } else if (intsetValueFitsInt32(value)) {
        /* Add to int32 array */
        uint64_t pos32;
        intsetSearchResult result = intsetFullFind32(f, (int32_t)value, &pos32);

        if (result == INTSET_FOUND) {
            if (added) {
                *added = false;
            }
            return f;
        }

        /* Insert at pos32 */
        uint64_t newCount = f->count32 + 1;
        int32_t *newValues = zcalloc(newCount, sizeof(int32_t));

        /* Copy before insert point */
        if (pos32 > 0) {
            memcpy(newValues, f->values32, pos32 * sizeof(int32_t));
        }

        /* Insert new value */
        newValues[pos32] = (int32_t)value;

        /* Copy after insert point */
        if (pos32 < f->count32) {
            memcpy(&newValues[pos32 + 1], &f->values32[pos32],
                   (f->count32 - pos32) * sizeof(int32_t));
        }

        if (f->values32) {
            zfree(f->values32);
        }
        f->values32 = newValues;
        f->count32 = newCount;

    } else {
        /* Add to int64 array */
        uint64_t pos64;
        intsetSearchResult result = intsetFullFind64(f, value, &pos64);

        if (result == INTSET_FOUND) {
            if (added) {
                *added = false;
            }
            return f;
        }

        /* Insert at pos64 */
        uint64_t newCount = f->count64 + 1;
        int64_t *newValues = zcalloc(newCount, sizeof(int64_t));

        /* Copy before insert point */
        if (pos64 > 0) {
            memcpy(newValues, f->values64, pos64 * sizeof(int64_t));
        }

        /* Insert new value */
        newValues[pos64] = value;

        /* Copy after insert point */
        if (pos64 < f->count64) {
            memcpy(&newValues[pos64 + 1], &f->values64[pos64],
                   (f->count64 - pos64) * sizeof(int64_t));
        }

        if (f->values64) {
            zfree(f->values64);
        }
        f->values64 = newValues;
        f->count64 = newCount;
    }

    if (added) {
        *added = true;
    }
    return f;
}

/* Remove value from full intset */
intsetFull *intsetFullRemove(intsetFull *f, int64_t value, bool *removed) {
    if (!f) {
        if (removed) {
            *removed = false;
        }
        return NULL;
    }

    if (intsetValueFitsInt16(value)) {
        /* Remove from int16 array */
        uint64_t pos16;
        intsetSearchResult result = intsetFullFind16(f, (int16_t)value, &pos16);

        if (result == INTSET_NOT_FOUND) {
            if (removed) {
                *removed = false;
            }
            return f;
        }

        /* Remove at pos16 */
        if (f->count16 == 1) {
            /* Last element */
            zfree(f->values16);
            f->values16 = NULL;
            f->count16 = 0;
        } else {
            uint64_t newCount = f->count16 - 1;
            int16_t *newValues = zcalloc(newCount, sizeof(int16_t));

            /* Copy before removed element */
            if (pos16 > 0) {
                memcpy(newValues, f->values16, pos16 * sizeof(int16_t));
            }

            /* Copy after removed element */
            if (pos16 < f->count16 - 1) {
                memcpy(&newValues[pos16], &f->values16[pos16 + 1],
                       (f->count16 - pos16 - 1) * sizeof(int16_t));
            }

            zfree(f->values16);
            f->values16 = newValues;
            f->count16 = newCount;
        }

    } else if (intsetValueFitsInt32(value)) {
        /* Remove from int32 array */
        uint64_t pos32;
        intsetSearchResult result = intsetFullFind32(f, (int32_t)value, &pos32);

        if (result == INTSET_NOT_FOUND) {
            if (removed) {
                *removed = false;
            }
            return f;
        }

        /* Remove at pos32 */
        if (f->count32 == 1) {
            /* Last element */
            zfree(f->values32);
            f->values32 = NULL;
            f->count32 = 0;
        } else {
            uint64_t newCount = f->count32 - 1;
            int32_t *newValues = zcalloc(newCount, sizeof(int32_t));

            /* Copy before removed element */
            if (pos32 > 0) {
                memcpy(newValues, f->values32, pos32 * sizeof(int32_t));
            }

            /* Copy after removed element */
            if (pos32 < f->count32 - 1) {
                memcpy(&newValues[pos32], &f->values32[pos32 + 1],
                       (f->count32 - pos32 - 1) * sizeof(int32_t));
            }

            zfree(f->values32);
            f->values32 = newValues;
            f->count32 = newCount;
        }

    } else {
        /* Remove from int64 array */
        uint64_t pos64;
        intsetSearchResult result = intsetFullFind64(f, value, &pos64);

        if (result == INTSET_NOT_FOUND) {
            if (removed) {
                *removed = false;
            }
            return f;
        }

        /* Remove at pos64 */
        if (f->count64 == 1) {
            /* Last element */
            zfree(f->values64);
            f->values64 = NULL;
            f->count64 = 0;
        } else {
            uint64_t newCount = f->count64 - 1;
            int64_t *newValues = zcalloc(newCount, sizeof(int64_t));

            /* Copy before removed element */
            if (pos64 > 0) {
                memcpy(newValues, f->values64, pos64 * sizeof(int64_t));
            }

            /* Copy after removed element */
            if (pos64 < f->count64 - 1) {
                memcpy(&newValues[pos64], &f->values64[pos64 + 1],
                       (f->count64 - pos64 - 1) * sizeof(int64_t));
            }

            zfree(f->values64);
            f->values64 = newValues;
            f->count64 = newCount;
        }
    }

    if (removed) {
        *removed = true;
    }
    return f;
}

/* Initialize iterator */
void intsetFullIteratorInit(intsetFullIterator *it, const intsetFull *f) {
    it->f = f;
    it->pos16 = 0;
    it->pos32 = 0;
    it->pos64 = 0;
}

/* Get next value from iterator - performs virtual 3-way merge */
bool intsetFullIteratorNext(intsetFullIterator *it, int64_t *value) {
    if (!it || !it->f) {
        return false;
    }

    /* Virtual merge of three sorted arrays */
    bool has16 = it->pos16 < it->f->count16;
    bool has32 = it->pos32 < it->f->count32;
    bool has64 = it->pos64 < it->f->count64;

    if (!has16 && !has32 && !has64) {
        return false;
    }

    /* Find minimum among available values */
    int64_t val16 = has16 ? it->f->values16[it->pos16] : INT64_MAX;
    int64_t val32 = has32 ? it->f->values32[it->pos32] : INT64_MAX;
    int64_t val64 = has64 ? it->f->values64[it->pos64] : INT64_MAX;

    if (val16 <= val32 && val16 <= val64) {
        /* Take from int16 array */
        if (value) {
            *value = val16;
        }
        it->pos16++;
    } else if (val32 <= val64) {
        /* Take from int32 array */
        if (value) {
            *value = val32;
        }
        it->pos32++;
    } else {
        /* Take from int64 array */
        if (value) {
            *value = val64;
        }
        it->pos64++;
    }

    return true;
}
