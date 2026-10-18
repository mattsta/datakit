#include "bbits.h"

#include "cleaner.h"
#include "datakit.h"
#include <math.h> /* sqrt */

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint64_t *, zfree, U64);
defcleanerName(double *, zfree, Doubles);

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
#define DOD_DIV_CEIL(a, b) (((a) + (b) - 1) / (b))
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

            /* conform allocation size of unused space
             * Note: Add 8 bytes padding because varintBitstreamGet may read
             * up to 8 bytes ahead when bits span word boundaries */
            const size_t kwCeil = DOD_DIV_CEIL(kw->usedBits, 8) + 8;
            const size_t vwCeil = DOD_DIV_CEIL(vw->usedBits, 8) + 8;
            if (kwCeil < BYTES_PER_BITMAP) {
                /* @own: container; free=zfree */
                kw->d = zrealloc(kw->d, kwCeil);
            }

            if (vwCeil < BYTES_PER_BITMAP) {
                /* @own: container; free=zfree */
                vw->d = zrealloc(vw->d, vwCeil);
            }
        }

        /* Increase array length count */
        dd->count++;

        /* Grow arrays */
        /* @own: container; free=zfree */
        dd->key = zrealloc(dd->key, sizeof(*dd->key) * dd->count);
        /* @own: container; free=zfree */
        dd->val = zrealloc(dd->val, sizeof(*dd->val) * dd->count);

        /* Grow original t0/t1 storage arrays */
        /* @own: container; free=zfree */
        dd->keyT0 = zrealloc(dd->keyT0, sizeof(*dd->keyT0) * dd->count);
        /* @own: container; free=zfree */
        dd->keyT1 = zrealloc(dd->keyT1, sizeof(*dd->keyT1) * dd->count);
        /* @own: container; free=zfree */
        dd->valT0 = zrealloc(dd->valT0, sizeof(*dd->valT0) * dd->count);
        /* @own: container; free=zfree */
        dd->valT1 = zrealloc(dd->valT1, sizeof(*dd->valT1) * dd->count);

        /* Re-bind variables */
        kw = &dd->key[dd->count - 1];
        vw = &dd->val[dd->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){0};
        *vw = (dodWriter){0};

        /* @own: container; free=zfree */
        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        /* @own: container; free=zfree */
        vw->d = zcalloc(1, BYTES_PER_BITMAP);

        /* Initialize original t0/t1 - will be set on first two writes */
        dd->keyT0[dd->count - 1] = 0;
        dd->keyT1[dd->count - 1] = 0;
        dd->valT0[dd->count - 1] = 0;
        dd->valT1[dd->count - 1] = 0;
    }

    /* Capture original t0 and t1 values before they get rotated */
    size_t idx = dd->count - 1;
    if (kw->count == 0) {
        dd->keyT0[idx] = newKey;
        dd->valT0[idx] = newVal;
    } else if (kw->count == 1) {
        dd->keyT1[idx] = newKey;
        dd->valT1[idx] = newVal;
    }

    /* Now we can almost start working on something */
    dodWrite(kw, newKey);
    dodWrite(vw, newVal);

    dd->elements++;

    return true;
}

/* Helper: Read all values from a dodWrite() bitmap. A single value lives
 * only in the writer (stored here as t0); from two values on, the bitmap's
 * header holds the first two and dodReadAll() decodes the rest after it. */
static void bbitsReadDodAll(const dod *d, dodVal t0, dodVal t1, uint64_t *vals,
                            size_t elemCount) {
    if (elemCount == 0) {
        return;
    }

    if (elemCount == 1) {
        vals[0] = (uint64_t)t0;
        return;
    }

    dodReadAll(d, vals, elemCount);
    assert(vals[0] == (uint64_t)t0 && vals[1] == (uint64_t)t1);
    (void)t1;
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

    /* Nothing to read at or past the end */
    if ((size_t)offset >= dd->elements) {
        return false;
    }

    /* Nothing to read for a zero count */
    if (*count == 0) {
        return false;
    }

    /* A negative count, or one running past the end, reads to the end */
    const size_t available = dd->elements - (size_t)offset;
    if (*count < 0 || (size_t)*count > available) {
        *count = (ssize_t)available;
    }

    size_t currentCount = 0;
    /* Find first bitmap pair having 'offset' within it... */
    size_t i;
    for (i = 0; i < dd->count; i++) {
        assert(dd->key[i].count == dd->val[i].count);

        /* If offset is within this bitmap, stop here */
        if (currentCount + dd->key[i].count > (size_t)offset) {
            break;
        }

        currentCount += dd->key[i].count;
    }

    /* offset < elements, so the scan stopped inside a bitmap pair */
    assert(i < dd->count);
    kw = &dd->key[i];
    vw = &dd->val[i];

    /* startOffset is how far into this bitmap to start reading */
    size_t startOffset = (size_t)offset - currentCount;
    size_t totalWritten = 0;
    size_t remaining = (size_t)*count;

    /* @own: caller; free=zfree */
    *key = zmalloc(*count * sizeof(**key));
    /* @own: caller; free=zfree */
    *val = zmalloc(*count * sizeof(**val));

    /* Read from first bitmap starting at startOffset */
    size_t elementsAvailable = kw->count - startOffset;
    size_t elementsToRead =
        (elementsAvailable < remaining) ? elementsAvailable : remaining;

    {
        /* Read to temp buffer then copy needed portion */
        cleanerName(zfree, U64) uint64_t *tmpKey =
            zmalloc(kw->count * sizeof(*tmpKey));
        cleanerName(zfree, U64) uint64_t *tmpVal =
            zmalloc(vw->count * sizeof(*tmpVal));

        bbitsReadDodAll(kw->d, dd->keyT0[i], dd->keyT1[i], tmpKey, kw->count);
        bbitsReadDodAll(vw->d, dd->valT0[i], dd->valT1[i], tmpVal, vw->count);

        memcpy(*key, tmpKey + startOffset, elementsToRead * sizeof(**key));
        memcpy(*val, tmpVal + startOffset, elementsToRead * sizeof(**val));
    }

    totalWritten += elementsToRead;
    remaining -= elementsToRead;
    i++;

    /* Continue reading from subsequent bitmaps if needed */
    while (remaining > 0 && i < dd->count) {
        kw = &dd->key[i];
        vw = &dd->val[i];

        elementsToRead = (kw->count < remaining) ? kw->count : remaining;

        /* Read to temp buffer then copy needed portion */
        cleanerName(zfree, U64) uint64_t *tmpKey =
            zmalloc(kw->count * sizeof(*tmpKey));
        cleanerName(zfree, U64) uint64_t *tmpVal =
            zmalloc(vw->count * sizeof(*tmpVal));

        bbitsReadDodAll(kw->d, dd->keyT0[i], dd->keyT1[i], tmpKey, kw->count);
        bbitsReadDodAll(vw->d, dd->valT0[i], dd->valT1[i], tmpVal, vw->count);

        memcpy(*key + totalWritten, tmpKey, elementsToRead * sizeof(**key));
        memcpy(*val + totalWritten, tmpVal, elementsToRead * sizeof(**val));

        totalWritten += elementsToRead;
        remaining -= elementsToRead;
        i++;
    }

    if (mean && variance && stddev) {
        /* Welford: 'm2' accumulates squared deviations from the mean */
        double runMean = 0;
        double m2 = 0;
        for (size_t j = 0; j < (size_t)*count; j++) {
            const double x = (double)(*val)[j];
            const double delta = x - runMean;
            runMean += delta / (double)(j + 1);
            m2 += delta * (x - runMean);
        }

        /* Population statistics; *count > 0 here */
        *mean = runMean;
        *variance = m2 / (double)*count;
        *stddev = sqrt(*variance);
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
        zfree(dd->keyT0);
        zfree(dd->keyT1);
        zfree(dd->valT0);
        zfree(dd->valT1);
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

            /* conform allocation size of unused space
             * Note: Add 8 bytes padding because varintBitstreamGet may read
             * up to 8 bytes ahead when bits span word boundaries */
            const size_t kwCeil = DOD_DIV_CEIL(kw->usedBits, 8) + 8;
            const size_t vwCeil = DOD_DIV_CEIL(vw->usedBits, 8) + 8;
            if (kwCeil < BYTES_PER_BITMAP) {
                /* @own: container; free=zfree */
                kw->d = zrealloc(kw->d, kwCeil);
            }

            if (vwCeil < BYTES_PER_BITMAP) {
                /* @own: container; free=zfree */
                vw->d = zrealloc(vw->d, vwCeil);
            }
        }

        /* Increase array length count */
        dx->count++;

        /* Grow arrays */
        /* @own: container; free=zfree */
        dx->key = zrealloc(dx->key, sizeof(*dx->key) * dx->count);
        /* @own: container; free=zfree */
        dx->val = zrealloc(dx->val, sizeof(*dx->val) * dx->count);

        /* Grow original t0/t1 storage arrays for keys */
        /* @own: container; free=zfree */
        dx->keyT0 = zrealloc(dx->keyT0, sizeof(*dx->keyT0) * dx->count);
        /* @own: container; free=zfree */
        dx->keyT1 = zrealloc(dx->keyT1, sizeof(*dx->keyT1) * dx->count);

        /* Re-bind variables */
        kw = &dx->key[dx->count - 1];
        vw = &dx->val[dx->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){0};
        *vw = (xofWriter){.currentLeadingZeroes = -1,
                          .currentTrailingZeroes = -1};

        /* @own: container; free=zfree */
        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        /* @own: container; free=zfree */
        vw->d = zcalloc(1, BYTES_PER_BITMAP);

        /* Initialize original t0/t1 for keys */
        dx->keyT0[dx->count - 1] = 0;
        dx->keyT1[dx->count - 1] = 0;
    }

    /* Capture original t0 and t1 values for keys before rotation */
    size_t idx = dx->count - 1;
    if (kw->count == 0) {
        dx->keyT0[idx] = newKey;
    } else if (kw->count == 1) {
        dx->keyT1[idx] = newKey;
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
        zfree(dx->keyT0);
        zfree(dx->keyT1);
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

    /* Nothing to read at or past the end */
    if ((size_t)offset >= dx->elements) {
        return false;
    }

    /* Nothing to read for a zero count */
    if (*count == 0) {
        return false;
    }

    /* A negative count, or one running past the end, reads to the end */
    const size_t available = dx->elements - (size_t)offset;
    if (*count < 0 || (size_t)*count > available) {
        *count = (ssize_t)available;
    }

    size_t currentCount = 0;
    /* Find first bitmap pair having 'offset' within it... */
    size_t i;
    for (i = 0; i < dx->count; i++) {
        assert(dx->key[i].count == dx->val[i].count);

        /* If offset is within this bitmap, stop here */
        if (currentCount + dx->key[i].count > (size_t)offset) {
            break;
        }

        currentCount += dx->key[i].count;
    }

    /* offset < elements, so the scan stopped inside a bitmap pair */
    assert(i < dx->count);
    kw = &dx->key[i];
    vw = &dx->val[i];

    /* startOffset is how far into this bitmap to start reading */
    size_t startOffset = (size_t)offset - currentCount;
    size_t totalWritten = 0;
    size_t remaining = (size_t)*count;

    /* @own: caller; free=zfree */
    *key = zmalloc(*count * sizeof(**key));
    /* @own: caller; free=zfree */
    *val = zmalloc(*count * sizeof(**val));

    /* Read from first bitmap starting at startOffset */
    size_t elementsAvailable = kw->count - startOffset;
    size_t elementsToRead =
        (elementsAvailable < remaining) ? elementsAvailable : remaining;

    {
        /* Read to temp buffers then copy needed portion */
        cleanerName(zfree, U64) uint64_t *tmpKey =
            zmalloc(kw->count * sizeof(*tmpKey));
        cleanerName(zfree, Doubles) double *tmpVal =
            zmalloc(vw->count * sizeof(*tmpVal));

        bbitsReadDodAll(kw->d, dx->keyT0[i], dx->keyT1[i], tmpKey, kw->count);
        xofReadAll(vw->d, tmpVal, vw->count);

        memcpy(*key, tmpKey + startOffset, elementsToRead * sizeof(**key));
        memcpy(*val, tmpVal + startOffset, elementsToRead * sizeof(**val));
    }

    totalWritten += elementsToRead;
    remaining -= elementsToRead;
    i++;

    /* Continue reading from subsequent bitmaps if needed */
    while (remaining > 0 && i < dx->count) {
        kw = &dx->key[i];
        vw = &dx->val[i];

        elementsToRead = (kw->count < remaining) ? kw->count : remaining;

        /* Read to temp buffers then copy needed portion */
        cleanerName(zfree, U64) uint64_t *tmpKey =
            zmalloc(kw->count * sizeof(*tmpKey));
        cleanerName(zfree, Doubles) double *tmpVal =
            zmalloc(vw->count * sizeof(*tmpVal));

        bbitsReadDodAll(kw->d, dx->keyT0[i], dx->keyT1[i], tmpKey, kw->count);
        xofReadAll(vw->d, tmpVal, vw->count);

        memcpy(*key + totalWritten, tmpKey, elementsToRead * sizeof(**key));
        memcpy(*val + totalWritten, tmpVal, elementsToRead * sizeof(**val));

        totalWritten += elementsToRead;
        remaining -= elementsToRead;
        i++;
    }

    if (mean && variance && stddev) {
        /* Welford: 'm2' accumulates squared deviations from the mean */
        double runMean = 0;
        double m2 = 0;
        for (size_t j = 0; j < (size_t)*count; j++) {
            const double x = (*val)[j];
            const double delta = x - runMean;
            runMean += delta / (double)(j + 1);
            m2 += delta * (x - runMean);
        }

        /* Population statistics; *count > 0 here */
        *mean = runMean;
        *variance = m2 / (double)*count;
        *stddev = sqrt(*variance);
    }

    return true;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int bbitsTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* Test bbitsDodDod end-to-end write and read verification */
    printf("Testing bbitsDodDod write/read with value verification...\n");
    {
        bbitsDodDod dd = {0};
        const size_t numElements = 100;

        /* Write key-value pairs */
        for (size_t i = 0; i < numElements; i++) {
            dodVal key = (dodVal)(i * 1000);
            dodVal val = (dodVal)(i * 2);
            bool result = bbitsDodDodAppend(&dd, key, val);
            if (!result) {
                ERR("bbitsDodDodAppend failed at i=%zu", i);
            }
        }

        if (dd.elements != numElements) {
            ERR("Expected %zu elements, got %zu", numElements, dd.elements);
        }

        /* Read back and verify all values */
        ssize_t count = -1;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;
        double mean, variance, stddev;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount failed%s", "");
        }

        if (count != (ssize_t)numElements) {
            ERR("Expected count %zu, got %zd", numElements, count);
        }

        /* Verify each value matches what we wrote */
        for (size_t i = 0; i < numElements; i++) {
            uint64_t expectedKey = i * 1000;
            uint64_t expectedVal = i * 2;

            if (keys[i] != expectedKey) {
                ERR("Key mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            if (vals[i] != expectedVal) {
                ERR("Val mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedVal, vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
        bbitsDodDodFree(&dd);
    }

    /* Test bbitsDodDod with larger dataset spanning multiple bitmap segments */
    printf("Testing bbitsDodDod with large dataset (multiple segments)...\n");
    {
        bbitsDodDod dd = {0};
        const size_t numElements = 5000;

        /* Write key-value pairs */
        for (size_t i = 0; i < numElements; i++) {
            dodVal key = (dodVal)(i * 100);
            dodVal val = (dodVal)(i * 3 + 7);
            bool result = bbitsDodDodAppend(&dd, key, val);
            if (!result) {
                ERR("bbitsDodDodAppend failed at i=%zu", i);
            }
        }

        if (dd.elements != numElements) {
            ERR("Expected %zu elements, got %zu", numElements, dd.elements);
        }

        printf("  Created %zu bitmap segments for %zu elements\n", dd.count,
               numElements);

        /* Read back and verify all values */
        ssize_t count = -1;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                NULL, NULL, NULL);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount failed%s", "");
        }

        if (count != (ssize_t)numElements) {
            ERR("Expected count %zu, got %zd", numElements, count);
        }

        /* Verify each value */
        for (size_t i = 0; i < numElements; i++) {
            uint64_t expectedKey = i * 100;
            uint64_t expectedVal = i * 3 + 7;

            if (keys[i] != expectedKey) {
                ERR("Key mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            if (vals[i] != expectedVal) {
                ERR("Val mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedVal, vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
        bbitsDodDodFree(&dd);
    }

    /* Test bbitsDodXof end-to-end write and read verification */
    printf("Testing bbitsDodXof write/read with value verification...\n");
    {
        bbitsDodXof dx = {0};
        const size_t numElements = 100;

        /* Write key-double pairs */
        for (size_t i = 0; i < numElements; i++) {
            dodVal key = (dodVal)(i * 1000);
            double val = (double)i * 1.5 + 0.25;
            bool result = bbitsDodXofAppend(&dx, key, val);
            if (!result) {
                ERR("bbitsDodXofAppend failed at i=%zu", i);
            }
        }

        if (dx.elements != numElements) {
            ERR("Expected %zu elements, got %zu", numElements, dx.elements);
        }

        /* Read back and verify all values */
        ssize_t count = -1;
        uint64_t *keys = NULL;
        double *vals = NULL;
        double mean, variance, stddev;

        bool result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodXofGetOffsetCount failed%s", "");
        }

        if (count != (ssize_t)numElements) {
            ERR("Expected count %zu, got %zd", numElements, count);
        }

        /* Verify each value matches what we wrote */
        for (size_t i = 0; i < numElements; i++) {
            uint64_t expectedKey = i * 1000;
            double expectedVal = (double)i * 1.5 + 0.25;

            if (keys[i] != expectedKey) {
                ERR("Key mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            /* Compare doubles with small epsilon for floating point errors */
            if (fabs(vals[i] - expectedVal) > 1e-10) {
                ERR("Val mismatch at %zu: expected %f, got %f", i, expectedVal,
                    vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
        bbitsDodXofFree(&dx);
    }

    /* Test bbitsDodXof with larger dataset spanning multiple segments */
    printf("Testing bbitsDodXof with large dataset (multiple segments)...\n");
    {
        bbitsDodXof dx = {0};
        const size_t numElements = 5000;

        /* Write key-double pairs */
        for (size_t i = 0; i < numElements; i++) {
            dodVal key = (dodVal)(i * 100);
            double val = (double)i * 0.123 + 42.0;
            bool result = bbitsDodXofAppend(&dx, key, val);
            if (!result) {
                ERR("bbitsDodXofAppend failed at i=%zu", i);
            }
        }

        if (dx.elements != numElements) {
            ERR("Expected %zu elements, got %zu", numElements, dx.elements);
        }

        printf("  Created %zu bitmap segments for %zu elements\n", dx.count,
               numElements);

        /* Read back and verify all values */
        ssize_t count = -1;
        uint64_t *keys = NULL;
        double *vals = NULL;

        bool result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &vals,
                                                NULL, NULL, NULL);
        if (!result) {
            ERR("bbitsDodXofGetOffsetCount failed%s", "");
        }

        if (count != (ssize_t)numElements) {
            ERR("Expected count %zu, got %zd", numElements, count);
        }

        /* Verify each value */
        for (size_t i = 0; i < numElements; i++) {
            uint64_t expectedKey = i * 100;
            double expectedVal = (double)i * 0.123 + 42.0;

            if (keys[i] != expectedKey) {
                ERR("Key mismatch at %zu: expected %" PRIu64 ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            if (fabs(vals[i] - expectedVal) > 1e-10) {
                ERR("Val mismatch at %zu: expected %f, got %f", i, expectedVal,
                    vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
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
            ERR("bbitsDodDodGetOffsetCount should return false for empty%s",
                "");
        }

        bbitsDodXof dx = {0};
        double *dvals = NULL;

        result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &dvals, NULL,
                                           NULL, NULL);
        if (result) {
            ERR("bbitsDodXofGetOffsetCount should return false for empty%s",
                "");
        }
    }

    /* Test offset reading */
    printf("Testing offset-based reading...\n");
    {
        bbitsDodDod dd = {0};
        const size_t numElements = 50;

        /* Write data */
        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)(i * 10), (dodVal)(i + 100));
        }

        /* Read with offset */
        ssize_t count = 10;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;

        bool result = bbitsDodDodGetOffsetCount(&dd, 20, &count, &keys, &vals,
                                                NULL, NULL, NULL);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount with offset failed%s", "");
        }

        /* Verify we got elements starting at offset 20 */
        for (size_t i = 0; i < (size_t)count && i < 10; i++) {
            size_t srcIdx = 20 + i;
            uint64_t expectedKey = srcIdx * 10;
            uint64_t expectedVal = srcIdx + 100;

            if (keys[i] != expectedKey) {
                ERR("Offset key mismatch at %zu: expected %" PRIu64
                    ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            if (vals[i] != expectedVal) {
                ERR("Offset val mismatch at %zu: expected %" PRIu64
                    ", got %" PRIu64,
                    i, expectedVal, vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
        bbitsDodDodFree(&dd);
    }

    /* Test free on empty structures */
    printf("Testing free on empty structures...\n");
    {
        bbitsDodDod dd = {0};
        bbitsDodDodFree(&dd); /* Should not crash */

        bbitsDodXof dx = {0};
        bbitsDodXofFree(&dx); /* Should not crash */
    }

    /* Test negative offset (from end) */
    printf("Testing negative offset (from end)...\n");
    {
        bbitsDodDod dd = {0};
        const size_t numElements = 100;

        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)(i * 5), (dodVal)(i * 7));
        }

        /* Read last 10 elements using negative offset */
        ssize_t count = 10;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;

        bool result = bbitsDodDodGetOffsetCount(&dd, -10, &count, &keys, &vals,
                                                NULL, NULL, NULL);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount with negative offset failed%s", "");
        }

        /* Verify we got last 10 elements */
        for (size_t i = 0; i < (size_t)count; i++) {
            size_t srcIdx = 90 + i;
            uint64_t expectedKey = srcIdx * 5;
            uint64_t expectedVal = srcIdx * 7;

            if (keys[i] != expectedKey) {
                ERR("Neg offset key mismatch at %zu: expected %" PRIu64
                    ", got %" PRIu64,
                    i, expectedKey, keys[i]);
            }

            if (vals[i] != expectedVal) {
                ERR("Neg offset val mismatch at %zu: expected %" PRIu64
                    ", got %" PRIu64,
                    i, expectedVal, vals[i]);
            }
        }

        zfree(keys);
        zfree(vals);
        bbitsDodDodFree(&dd);
    }

    /* Test statistics calculation */
    printf("Testing statistics calculation...\n");
    {
        bbitsDodDod dd = {0};

        /* Write known values: 10, 20, 30, 40, 50 - mean should be 30 */
        bbitsDodDodAppend(&dd, 1, 10);
        bbitsDodDodAppend(&dd, 2, 20);
        bbitsDodDodAppend(&dd, 3, 30);
        bbitsDodDodAppend(&dd, 4, 40);
        bbitsDodDodAppend(&dd, 5, 50);

        ssize_t count = -1;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;
        double mean, variance, stddev;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount for stats failed%s", "");
        }

        /* Expected mean = (10+20+30+40+50)/5 = 30 */
        if (fabs(mean - 30.0) > 1e-10) {
            ERR("Mean mismatch: expected 30.0, got %f", mean);
        }

        /* Population variance = (400+100+0+100+400)/5 = 200 */
        if (fabs(variance - 200.0) > 1e-9 ||
            fabs(stddev - sqrt(200.0)) > 1e-9) {
            ERR("Variance/stddev mismatch: got %f/%f, expected 200/%f",
                variance, stddev, sqrt(200.0));
        }

        zfree(keys);
        zfree(vals);
        bbitsDodDodFree(&dd);
    }

    /* Reads that start at or past the end are refused; reads that run past
     * the end stop at the end and report the shorter count. */
    printf("Testing offset/count range clamping...\n");
    {
        /* Irregular values so the DoD stream spills into several segments */
#define BBITS_CLAMP_VAL(i) (((uint64_t)(i) * 2654435761u) % 1000003)
        const size_t numElements = 5000;
        bbitsDodDod dd = {0};
        bbitsDodXof dx = {0};
        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)(i * 100),
                              (dodVal)BBITS_CLAMP_VAL(i));
            bbitsDodXofAppend(&dx, (dodVal)(i * 100), (double)i * 0.123 + 42.0);
        }

        if (dd.count < 2 || dx.count < 2) {
            ERR("Expected multiple segments, got %zu and %zu", dd.count,
                dx.count);
        }

        const ssize_t pastEnd[] = {(ssize_t)numElements,
                                   (ssize_t)numElements + 7};
        for (size_t p = 0; p < sizeof(pastEnd) / sizeof(*pastEnd); p++) {
            ssize_t count = 1;
            uint64_t *keys = NULL;
            uint64_t *vals = NULL;
            double *dvals = NULL;
            if (bbitsDodDodGetOffsetCount(&dd, pastEnd[p], &count, &keys, &vals,
                                          NULL, NULL, NULL)) {
                ERR("DodDod offset %zd should be refused", pastEnd[p]);
                zfree(keys);
                zfree(vals);
            }

            count = 1;
            if (bbitsDodXofGetOffsetCount(&dx, pastEnd[p], &count, &keys,
                                          &dvals, NULL, NULL, NULL)) {
                ERR("DodXof offset %zd should be refused", pastEnd[p]);
                zfree(keys);
                zfree(dvals);
            }
        }

        /* {offset, requested count, count expected back} */
        const ssize_t cases[][3] = {
            {(ssize_t)numElements - 10, -1, 10},
            {(ssize_t)numElements - 10, 50, 10},
            {-10, 50, 10},
            {1000, (ssize_t)numElements, (ssize_t)numElements - 1000},
            {(ssize_t)numElements - 1, 1, 1},
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
            const size_t start = cases[c][0] < 0
                                     ? numElements + (size_t)cases[c][0]
                                     : (size_t)cases[c][0];
            const size_t want = (size_t)cases[c][2];

            ssize_t count = cases[c][1];
            uint64_t *keys = NULL;
            uint64_t *vals = NULL;
            double mean = 0, variance = 0, stddev = 0;
            if (!bbitsDodDodGetOffsetCount(&dd, cases[c][0], &count, &keys,
                                           &vals, &mean, &variance, &stddev)) {
                ERR("DodDod case %zu refused", c);
                continue;
            }

            if ((size_t)count != want) {
                ERR("DodDod case %zu: count %zd, expected %zu", c, count, want);
            }

            double sum = 0;
            for (size_t j = 0; j < want && j < (size_t)count; j++) {
                const size_t src = start + j;
                if (keys[j] != src * 100 || vals[j] != BBITS_CLAMP_VAL(src)) {
                    ERR("DodDod case %zu: wrong pair at %zu", c, j);
                    break;
                }
                sum += (double)BBITS_CLAMP_VAL(src);
            }

            if (fabs(mean - sum / (double)want) > 1e-6) {
                ERR("DodDod case %zu: mean %f, expected %f", c, mean,
                    sum / (double)want);
            }

            zfree(keys);
            zfree(vals);

            count = cases[c][1];
            double *dvals = NULL;
            if (!bbitsDodXofGetOffsetCount(&dx, cases[c][0], &count, &keys,
                                           &dvals, NULL, NULL, NULL)) {
                ERR("DodXof case %zu refused", c);
                continue;
            }

            if ((size_t)count != want) {
                ERR("DodXof case %zu: count %zd, expected %zu", c, count, want);
            }

            for (size_t j = 0; j < want && j < (size_t)count; j++) {
                const size_t src = start + j;
                if (keys[j] != src * 100 ||
                    dvals[j] != (double)src * 0.123 + 42.0) {
                    ERR("DodXof case %zu: wrong pair at %zu", c, j);
                    break;
                }
            }

            zfree(keys);
            zfree(dvals);
        }

        bbitsDodDodFree(&dd);
        bbitsDodXofFree(&dx);
#undef BBITS_CLAMP_VAL
    }

    /* mean/variance/stddev are population statistics; check them against a
     * two-pass computation over the generated values themselves */
    printf("Testing statistics against a two-pass oracle...\n");
    {
#define BBITS_STAT_DOD(i) (((uint64_t)(i) * 2654435761u) % 1000003)
#define BBITS_STAT_XOF(i) ((double)(i) * 0.123 - 77.0)
        const size_t numElements = 3000;
        bbitsDodDod dd = {0};
        bbitsDodXof dx = {0};
        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)i, (dodVal)BBITS_STAT_DOD(i));
            bbitsDodXofAppend(&dx, (dodVal)i, BBITS_STAT_XOF(i));
        }

        /* {offset, count}: whole set, a middle window, one element */
        const ssize_t windows[][2] = {{0, -1}, {700, 1500}, {2999, 1}};
        for (size_t w = 0; w < sizeof(windows) / sizeof(*windows); w++) {
            const size_t start = (size_t)windows[w][0];
            const size_t n =
                windows[w][1] < 0 ? numElements - start : (size_t)windows[w][1];

            for (int kind = 0; kind < 2; kind++) {
                double sum = 0;
                for (size_t j = 0; j < n; j++) {
                    sum += kind == 0 ? (double)BBITS_STAT_DOD(start + j)
                                     : BBITS_STAT_XOF(start + j);
                }
                const double wantMean = sum / (double)n;

                double sq = 0;
                for (size_t j = 0; j < n; j++) {
                    const double x = kind == 0
                                         ? (double)BBITS_STAT_DOD(start + j)
                                         : BBITS_STAT_XOF(start + j);
                    sq += (x - wantMean) * (x - wantMean);
                }
                const double wantVar = sq / (double)n;

                ssize_t count = windows[w][1];
                uint64_t *keys = NULL;
                uint64_t *vals = NULL;
                double *dvals = NULL;
                double mean = -1, variance = -1, stddev = -1;
                const bool ok =
                    kind == 0
                        ? bbitsDodDodGetOffsetCount(&dd, windows[w][0], &count,
                                                    &keys, &vals, &mean,
                                                    &variance, &stddev)
                        : bbitsDodXofGetOffsetCount(&dx, windows[w][0], &count,
                                                    &keys, &dvals, &mean,
                                                    &variance, &stddev);
                if (!ok || (size_t)count != n) {
                    ERR("window %zu kind %d: read failed", w, kind);
                } else {
                    const double tol = 1e-9 * (1 + fabs(wantVar));
                    if (fabs(mean - wantMean) > 1e-9 * (1 + fabs(wantMean)) ||
                        fabs(variance - wantVar) > tol ||
                        fabs(stddev - sqrt(wantVar)) > tol) {
                        ERR("window %zu kind %d: got %f/%f/%f, expected "
                            "%f/%f/%f",
                            w, kind, mean, variance, stddev, wantMean, wantVar,
                            sqrt(wantVar));
                    }
                }

                zfree(keys);
                zfree(vals);
                zfree(dvals);
            }
        }

        /* A zero count reads nothing and allocates nothing */
        ssize_t count = 0;
        uint64_t *keys = NULL;
        uint64_t *vals = NULL;
        double *dvals = NULL;
        double mean = -1, variance = -1, stddev = -1;
        if (bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals, &mean,
                                      &variance, &stddev) ||
            bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &dvals, &mean,
                                      &variance, &stddev) ||
            keys || vals || dvals) {
            ERRR("A zero count should be refused without allocating");
        }

        bbitsDodDodFree(&dd);
        bbitsDodXofFree(&dx);
#undef BBITS_STAT_DOD
#undef BBITS_STAT_XOF
    }

    TEST_FINAL_RESULT;
}
#endif
