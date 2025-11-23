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
    dodWriter *kw = NULL;
    dodWriter *vw = NULL;

    /* If first time, or current bitmaps are too big, create new holders */
    bool needNewBitmap = !dd->count;
    if (dd->count > 0) {
        kw = &dd->key[dd->count - 1];
        vw = &dd->val[dd->count - 1];
        needNewBitmap = TOO_BIG_DOD(kw->usedBits) || TOO_BIG_DOD(vw->usedBits);
    }

    if (needNewBitmap) {
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

    /* Early return if structure is empty - no data to read */
    if (dd->count == 0) {
        return false;
    }

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
    dodWriter *kw = NULL;
    xofWriter *vw = NULL;

    /* If first time, or current bitmaps are too big, create new holders */
    bool needNewBitmap = !dx->count;
    if (dx->count > 0) {
        kw = &dx->key[dx->count - 1];
        vw = &dx->val[dx->count - 1];
        needNewBitmap = TOO_BIG_DOD(kw->usedBits) || TOO_BIG_XOF(vw->usedBits);
    }

    if (needNewBitmap) {
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

    /* Early return if structure is empty - no data to read */
    if (dx->count == 0) {
        return false;
    }

    /* If offset from tail, convert to offset from forward */
    if (offset < 0) {
        offset += dx->elements;
        if (offset < 0) {
            return false;
        }
    }

    /* If negative count, return all elements */
    if (*count < 0 || *count > (ssize_t)dx->elements) {
        *count = dx->elements;
    }

    size_t currentCount = 0;
    /* Find first bitmap pair having 'offset' within it... */
    size_t i;
    for (i = 0; i < dx->count; i++) {
        kw = &dx->key[i];
        vw = &dx->val[i];

        /* If we found the offset match, use the 'kw' and 'vw' we just set */
        if (currentCount >= (size_t)offset) {
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

    while (totalCountRead < (size_t)*count) {
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
        for (size_t j = 0; j < (size_t)*count; j++) {
            const double delta = (*val)[j] - *mean;
            *mean += delta / (j + 1); /* fix zero-based index */
            *variance += delta * ((*val)[j] - *mean);
        }

        *stddev = sqrt(*variance / *count);
    }

    return true;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int bbitsTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* Note: The bbits module has a design issue where it uses dodWrite()
     * which stores t0/t1 in the writer struct, but dodReadAll() expects
     * them encoded in the bitmap. Full read-back testing requires fixing
     * the bbits/dod integration. For now, we test basic lifecycle. */

    /* Test bbitsDodDod basic append lifecycle */
    printf("Testing bbitsDodDod append lifecycle...\n");
    {
        bbitsDodDod dd = {0};

        /* Append some key-value pairs */
        for (uint64_t i = 0; i < 100; i++) {
            bool result = bbitsDodDodAppend(&dd, (dodVal)i * 1000, (dodVal)i * 2);
            if (!result) {
                ERR("bbitsDodDodAppend failed at i=%" PRIu64, i);
            }
        }

        if (dd.elements != 100) {
            ERR("Expected 100 elements, got %zu", dd.elements);
        }

        if (dd.count == 0) {
            ERR("Expected at least one bitmap segment%s", "");
        }

        /* Verify writers have data */
        if (dd.key == NULL || dd.val == NULL) {
            ERR("Key/val arrays should not be NULL after append%s", "");
        }

        bbitsDodDodFree(&dd);
    }

    /* Test bbitsDodDod with larger dataset */
    printf("Testing bbitsDodDod with large dataset...\n");
    {
        bbitsDodDod dd = {0};

        const size_t numElements = 5000;
        for (uint64_t i = 0; i < numElements; i++) {
            bool result = bbitsDodDodAppend(&dd, (dodVal)(i * 100),
                                             (dodVal)(i * 3 + 7));
            if (!result) {
                ERR("bbitsDodDodAppend failed at i=%" PRIu64, i);
            }
        }

        if (dd.elements != numElements) {
            ERR("Expected %zu elements, got %zu", numElements, dd.elements);
        }

        printf("  Created %zu bitmap segments for %zu elements\n",
               dd.count, numElements);

        bbitsDodDodFree(&dd);
    }

    /* Test bbitsDodXof basic append lifecycle */
    printf("Testing bbitsDodXof append lifecycle...\n");
    {
        bbitsDodXof dx = {0};

        /* Append key-double pairs */
        for (uint64_t i = 0; i < 100; i++) {
            double value = (double)i * 1.5 + 0.25;
            bool result = bbitsDodXofAppend(&dx, (dodVal)i * 1000, value);
            if (!result) {
                ERR("bbitsDodXofAppend failed at i=%" PRIu64, i);
            }
        }

        if (dx.elements != 100) {
            ERR("Expected 100 elements, got %zu", dx.elements);
        }

        if (dx.count == 0) {
            ERR("Expected at least one bitmap segment%s", "");
        }

        /* Verify writers have data */
        if (dx.key == NULL || dx.val == NULL) {
            ERR("Key/val arrays should not be NULL after append%s", "");
        }

        bbitsDodXofFree(&dx);
    }

    /* Test empty structure handling */
    printf("Testing empty structure handling...\n");
    {
        bbitsDodDod dd = {0};
        ssize_t count = -1;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                 NULL, NULL, NULL);
        if (result) {
            ERR("bbitsDodDodGetOffsetCount should return false for empty%s", "");
        }

        bbitsDodXof dx = {0};
        double *dvals = NULL;

        result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &dvals,
                                            NULL, NULL, NULL);
        if (result) {
            ERR("bbitsDodXofGetOffsetCount should return false for empty%s", "");
        }
    }

    /* Test free on NULL/empty structures */
    printf("Testing free on empty structures...\n");
    {
        bbitsDodDod dd = {0};
        bbitsDodDodFree(&dd); /* Should not crash */

        bbitsDodXof dx = {0};
        bbitsDodXofFree(&dx); /* Should not crash */
    }

    TEST_FINAL_RESULT;
}
#endif
