#include "bbits.h"

#include "datakit.h"
#include <math.h> /* sqrt */

#define BYTES_PER_BITMAP 4096

/* Check if bits in bitmap will grow larger than our allocation size */
#define TOO_BIG_(bits) ((bits) >= (BYTES_PER_BITMAP * 8))

/* For dod, the maximum size of an element is 72 bits (worst case) */
#define TOO_BIG_DOD(bits) TOO_BIG_((bits) + 72)

/* For xof, maximum size is sizeof(double) + overhead (worst case) */
#define TOO_BIG_XOF(bits) TOO_BIG_((bits) + (64 + (6 + 6 + 2)))

/* ====================================================================
 * Dod - Dod
 * ==================================================================== */
#define DOD_DIV_CEIL(a, b) (((a) + (b)-1) / (b))
bool bbitsDodDodAppend(bbitsDodDod *dd, const dodVal newKey,
                       const dodVal newVal) {
    /* Always use last elements of arrays */
    /* {k,v}w = {k,v}write */
    dodWriter *kw = &dd->key[dd->count - 1];
    dodWriter *vw = &dd->val[dd->count - 1];

    /* If current bitmaps are too big, create new holders
     * (or if this is the first time and we need to create everything...) */
    if ((TOO_BIG_DOD(kw->usedBits) || TOO_BIG_DOD(vw->usedBits)) ||
        !dd->count) {
        if (dd->count > 0) {
            dodCloseWrites(kw);
            dodCloseWrites(vw);

            /* conform allocation size of unused space */
            const size_t kwCeil = DOD_DIV_CEIL(kw->usedBits, 8);
            const size_t vwCeil = DOD_DIV_CEIL(vw->usedBits, 8);
            if (kwCeil < BYTES_PER_BITMAP) {
                kw->d = zrealloc(kw->d, kwCeil);
            }

            if (vwCeil < BYTES_PER_BITMAP) {
                vw->d = zrealloc(vw->d, vwCeil);
            }
        }

        /* Increase array length count */
        dd->count++;

        /* Grow arrays */
        dd->key = zrealloc(dd->key, sizeof(*dd->key) * dd->count);
        dd->val = zrealloc(dd->val, sizeof(*dd->val) * dd->count);

        /* Re-bind variables */
        kw = &dd->key[dd->count - 1];
        vw = &dd->val[dd->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){0};
        *vw = (dodWriter){0};

        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        vw->d = zcalloc(1, BYTES_PER_BITMAP);
    }

    /* Now we can almost start working on something */
    dodWrite(kw, newKey);
    dodWrite(vw, newVal);

    dd->elements++;

    return true;
}

bool bbitsDodDodGetOffsetCount(bbitsDodDod *dd, ssize_t offset,
                               ssize_t *const count, uint64_t **key,
                               uint64_t **val, double *mean, double *variance,
                               double *stddev) {
    dodWriter *kw;
    dodWriter *vw;

    /* If offset from tail, convert to offset from forward */
    if (offset < 0) {
        offset += dd->elements;
        if (offset < 0) {
            return false;
        }
    }

    /* If negative count, return all elements */
    if (*count < 0 || *count > dd->elements) {
        *count = dd->elements;
    }

    size_t currentCount = 0;
    /* Find first bitmap pair having 'offset' within it... */
    size_t i;
    for (i = 0; i < dd->count; i++) {
        kw = &dd->key[i];
        vw = &dd->val[i];

        /* If we found the offset match, use the 'kw' and 'vw' we just set */
        if (currentCount >= offset) {
            break;
        }

        assert(kw->count == vw->count);
        currentCount += kw->count;
    }

    size_t startReadingFirstBitsAtOffset = currentCount - offset;
    size_t totalCountRead = 0;

    /* Note: caller must free *key and *val */
    *key = zmalloc(*count * sizeof(**key));
    *val = zmalloc(*count * sizeof(**val));

    dodReadAll(kw->d, *key, kw->count);
    dodReadAll(vw->d, *val, vw->count);

    /* Now, if we have an offset inside this initial array (i.e. we don't want
     * to read from zero, but we are reading from +N offset), move the
     * offset index to the start of the array. */
    if (startReadingFirstBitsAtOffset > 0) {
        memmove(*key, *key + startReadingFirstBitsAtOffset,
                kw->count - startReadingFirstBitsAtOffset);
        memmove(*val, *val + startReadingFirstBitsAtOffset,
                kw->count - startReadingFirstBitsAtOffset);
    }

    /* Update accounting for our first one-off read */
    totalCountRead += (kw->count - startReadingFirstBitsAtOffset);
    i += 1;

    while (totalCountRead < *count) {
        /* We fixed the element count on entry to this function, so we will
         * never outrun our number of dod arrays... */
        assert(i < dd->count);

        /* Get next arrays... */
        kw = &dd->key[i];
        vw = &dd->val[i];

        /* Read into output... */
        dodReadAll(kw->d, *key + totalCountRead, kw->count);
        dodReadAll(vw->d, *val + totalCountRead, vw->count);

        /* Advance count so next read lands at the correct offset... */
        totalCountRead += kw->count;
        i++;
    }

    if (mean && variance && stddev) {
        *mean = 0;
        *variance = 0;
        for (size_t j = 0; j < *count; j++) {
            const double delta = (double)((*val)[j]) - *mean;
            *mean += delta / (j + 1); /* fix zero-based index */
            *variance += delta * ((*val)[j] - *mean);
        }

        *stddev = sqrt(*variance / *count);
    }

    return true;
}

void bbitsDodDodFree(bbitsDodDod *dd) {
    if (dd) {
        for (size_t i = 0; i < dd->count; i++) {
            dodWriter *kw = &dd->key[i];
            dodWriter *vw = &dd->val[i];

            zfree(kw->d);
            zfree(vw->d);
        }

        zfree(dd->key);
        zfree(dd->val);
    }
}

/* ====================================================================
 * Dod - Xof
 * ==================================================================== */
bool bbitsDodXofAppend(bbitsDodXof *dx, const dodVal newKey,
                       const double newVal) {
    /* Always use last elements of arrays */
    /* {k,v}w = {k,v}write */
    dodWriter *kw = &dx->key[dx->count - 1];
    xofWriter *vw = &dx->val[dx->count - 1];

    /* If current bitmaps are too big, create new holders
     * (or if this is the first time and we need to create everything...) */
    if ((TOO_BIG_DOD(kw->usedBits) || TOO_BIG_XOF(vw->usedBits)) ||
        !dx->count) {
        if (dx->count > 0) {
            dodCloseWrites(kw);
            //            dodCloseWrites(vw);

            /* conform allocation size of unused space */
            const size_t kwCeil = DOD_DIV_CEIL(kw->usedBits, 8);
            const size_t vwCeil = DOD_DIV_CEIL(vw->usedBits, 8);
            if (kwCeil < BYTES_PER_BITMAP) {
                kw->d = zrealloc(kw->d, kwCeil);
            }

            if (vwCeil < BYTES_PER_BITMAP) {
                vw->d = zrealloc(vw->d, vwCeil);
            }
        }

        /* Increase array length count */
        dx->count++;

        /* Grow arrays */
        dx->key = zrealloc(dx->key, sizeof(*dx->key) * dx->count);
        dx->val = zrealloc(dx->val, sizeof(*dx->val) * dx->count);

        /* Re-bind variables */
        kw = &dx->key[dx->count - 1];
        vw = &dx->val[dx->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){0};
        *vw = (xofWriter){.currentLeadingZeroes = -1,
                          .currentTrailingZeroes = -1};

        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        vw->d = zcalloc(1, BYTES_PER_BITMAP);
    }

    /* Now we can almost start working on something */
    dodWrite(kw, newKey);
    xofWrite(vw, newVal);

    /* Record addition of new element... */
    dx->elements++;

    return true;
}

void bbitsDodXofFree(bbitsDodXof *dx) {
    if (dx) {
        for (size_t i = 0; i < dx->count; i++) {
            dodWriter *kw = &dx->key[i];
            xofWriter *vw = &dx->val[i];

            zfree(kw->d);
            zfree(vw->d);
        }

        zfree(dx->key);
        zfree(dx->val);
    }
}

bool bbitsDodXofGetOffsetCount(bbitsDodXof *dx, ssize_t offset, ssize_t *count,
                               uint64_t **key, double **val, double *mean,
                               double *variance, double *stddev) {
    dodWriter *kw;
    xofWriter *vw;

    /* If offset from tail, convert to offset from forward */
    if (offset < 0) {
        offset += dx->elements;
        if (offset < 0) {
            return false;
        }
    }

    /* If negative count, return all elements */
    if (*count < 0 || *count > dx->elements) {
        *count = dx->elements;
    }

    size_t currentCount = 0;
    /* Find first bitmap pair having 'offset' within it... */
    size_t i;
    for (i = 0; i < dx->count; i++) {
        kw = &dx->key[i];
        vw = &dx->val[i];

        /* If we found the offset match, use the 'kw' and 'vw' we just set */
        if (currentCount >= offset) {
            break;
        }

        assert(kw->count == vw->count);
        currentCount += kw->count;
    }

    size_t startReadingFirstBitsAtOffset = currentCount - offset;
    size_t totalCountRead = 0;

    /* Note: caller must free *key and *val */
    *key = zmalloc(*count * sizeof(**key));
    *val = zmalloc(*count * sizeof(**val));

    dodReadAll(kw->d, *key, kw->count);
    xofReadAll(vw->d, *val, vw->count);

    /* Now, if we have an offset inside this initial array (i.e. we don't want
     * to read from zero, but we are reading from +N offset), move the
     * offset index to the start of the array. */
    if (startReadingFirstBitsAtOffset > 0) {
        memmove(*key, *key + startReadingFirstBitsAtOffset,
                kw->count - startReadingFirstBitsAtOffset);
        memmove(*val, *val + startReadingFirstBitsAtOffset,
                kw->count - startReadingFirstBitsAtOffset);
    }

    /* Update accounting for our first one-off read */
    totalCountRead += (kw->count - startReadingFirstBitsAtOffset);
    i += 1;

    while (totalCountRead < *count) {
        /* We fixed the element count on entry to this function, so we will
         * never outrun our number of dod arrays... */
        assert(i < dx->count);

        /* Get next arrays... */
        kw = &dx->key[i];
        vw = &dx->val[i];

        /* Read into output... */
        dodReadAll(kw->d, *key + totalCountRead, kw->count);
        xofReadAll(vw->d, *val + totalCountRead, vw->count);

        /* Advance count so next read lands at the correct offset... */
        totalCountRead += kw->count;
        i++;
    }

    if (mean && variance && stddev) {
        *mean = 0;
        *variance = 0;
        for (size_t j = 0; j < *count; j++) {
            const double delta = *val[j] - *mean;
            *mean += delta / (i + 1); /* fix zero-based index */
            *variance += delta * (*val[j] - *mean);
        }

        *stddev = sqrt(*variance / *count);
    }

    return true;
}
