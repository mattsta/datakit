#include "bbits.h"

#include "cleaner.h"
#include "datakit.h"
#include "dkBits.h" /* dkDivCeil */
#include <math.h>   /* sqrt */

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint64_t *, zfree, U64);
defcleanerName(double *, zfree, Doubles);

static constexpr int BYTES_PER_BITMAP = 4096;

/* Check if bits in bitmap will grow larger than our allocation size */
#define TOO_BIG_(bits) ((bits) >= (BYTES_PER_BITMAP * 8))

/* For dod, the maximum size of an element is 72 bits (worst case) */
#define TOO_BIG_DOD(bits) TOO_BIG_((bits) + 72)

/* For xof, maximum size is sizeof(double) + overhead (worst case) */
#define TOO_BIG_XOF(bits) TOO_BIG_((bits) + (64 + (6 + 6 + 2)))

/* ====================================================================
 * Dod - Dod
 * ==================================================================== */
bool bbitsDodDodAppend(bbitsDodDod *dd, const dodVal newKey,
                       const dodVal newVal) {
    /* Always use last elements of arrays */
    /* {k,v}w = {k,v}write */
    dodWriter *kw = nullptr;
    dodWriter *vw = nullptr;

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
            const size_t kwCeil = dkDivCeil(kw->usedBits, 8) + 8;
            const size_t vwCeil = dkDivCeil(vw->usedBits, 8) + 8;
            if (kwCeil < kw->totalBytes) {
                /* @own: container; free=zfree */
                kw->d = zrealloc(kw->d, kwCeil);
                kw->totalBytes = kwCeil;
            }

            if (vwCeil < vw->totalBytes) {
                /* @own: container; free=zfree */
                vw->d = zrealloc(vw->d, vwCeil);
                vw->totalBytes = vwCeil;
            }
        }

        /* Increase array length count */
        dd->count++;

        /* Grow arrays */
        /* @own: container; free=zfree */
        dd->key = zrealloc(dd->key, sizeof(*dd->key) * dd->count);
        /* @own: container; free=zfree */
        dd->val = zrealloc(dd->val, sizeof(*dd->val) * dd->count);

        /* Re-bind variables */
        kw = &dd->key[dd->count - 1];
        vw = &dd->val[dd->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){};
        *vw = (dodWriter){};

        /* The writers own these from here and grow them if they must */
        /* @own: container; free=zfree */
        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        kw->totalBytes = BYTES_PER_BITMAP;
        /* @own: container; free=zfree */
        vw->d = zcalloc(1, BYTES_PER_BITMAP);
        vw->totalBytes = BYTES_PER_BITMAP;
    }

    /* The writers keep the first two values themselves: a lone value in
     * t[0], and from two values on in the stream header. */
    dodWrite(kw, newKey);
    dodWrite(vw, newVal);

    dd->elements++;

    return true;
}

/* Helper: Read all values from a dodWrite() writer. A single value lives
 * only in the writer's t[0]; from two values on, the stream's header holds
 * the first two and dodReadAll() decodes the rest after it. */
static void bbitsReadDodAll(const dodWriter *w, uint64_t *vals) {
    if (w->count == 0) {
        return;
    }

    if (w->count == 1) {
        vals[0] = (uint64_t)w->t[0];
        return;
    }

    dodReadAll(w->d, vals, w->count);
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

        bbitsReadDodAll(kw, tmpKey);
        bbitsReadDodAll(vw, tmpVal);

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

        bbitsReadDodAll(kw, tmpKey);
        bbitsReadDodAll(vw, tmpVal);

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
    }
}

/* ====================================================================
 * Dod - Xof
 * ==================================================================== */
bool bbitsDodXofAppend(bbitsDodXof *dx, const dodVal newKey,
                       const double newVal) {
    /* Always use last elements of arrays */
    /* {k,v}w = {k,v}write */
    dodWriter *kw = nullptr;
    xofWriter *vw = nullptr;

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
            const size_t kwCeil = dkDivCeil(kw->usedBits, 8) + 8;
            const size_t vwCeil = dkDivCeil(vw->usedBits, 8) + 8;
            if (kwCeil < kw->totalBytes) {
                /* @own: container; free=zfree */
                kw->d = zrealloc(kw->d, kwCeil);
                kw->totalBytes = kwCeil;
            }

            if (vwCeil < vw->totalBytes) {
                /* @own: container; free=zfree */
                vw->d = zrealloc(vw->d, vwCeil);
                vw->totalBytes = vwCeil;
            }
        }

        /* Increase array length count */
        dx->count++;

        /* Grow arrays */
        /* @own: container; free=zfree */
        dx->key = zrealloc(dx->key, sizeof(*dx->key) * dx->count);
        /* @own: container; free=zfree */
        dx->val = zrealloc(dx->val, sizeof(*dx->val) * dx->count);

        /* Re-bind variables */
        kw = &dx->key[dx->count - 1];
        vw = &dx->val[dx->count - 1];

        /* Zero out new structs */
        *kw = (dodWriter){};
        *vw = (xofWriter){.currentLeadingZeroes = -1,
                          .currentTrailingZeroes = -1};

        /* The writers own these from here and grow them if they must */
        /* @own: container; free=zfree */
        kw->d = zcalloc(1, BYTES_PER_BITMAP);
        kw->totalBytes = BYTES_PER_BITMAP;
        /* @own: container; free=zfree */
        vw->d = zcalloc(1, BYTES_PER_BITMAP);
        vw->totalBytes = BYTES_PER_BITMAP;
    }

    /* The key writer keeps its first two values itself: a lone value in
     * t[0], and from two values on in the stream header. */
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

        bbitsReadDodAll(kw, tmpKey);
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

        bbitsReadDodAll(kw, tmpKey);
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

int bbitsTest(int, char *[]) {
    int32_t err = 0;

    /* Test bbitsDodDod end-to-end write and read verification */
    TEST("bbitsDodDod write/read with value verification") {
        bbitsDodDod dd = {};
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
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;
        double mean, variance, stddev;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount failed");
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
    TEST("bbitsDodDod with a large dataset (multiple segments)") {
        bbitsDodDod dd = {};
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
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                nullptr, nullptr, nullptr);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount failed");
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
    TEST("bbitsDodXof write/read with value verification") {
        bbitsDodXof dx = {};
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
        uint64_t *keys = nullptr;
        double *vals = nullptr;
        double mean, variance, stddev;

        bool result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodXofGetOffsetCount failed");
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
    TEST("bbitsDodXof with a large dataset (multiple segments)") {
        bbitsDodXof dx = {};
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
        uint64_t *keys = nullptr;
        double *vals = nullptr;

        bool result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &vals,
                                                nullptr, nullptr, nullptr);
        if (!result) {
            ERR("bbitsDodXofGetOffsetCount failed");
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
    TEST("empty structure handling") {
        bbitsDodDod dd = {};
        ssize_t count = -1;
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                nullptr, nullptr, nullptr);
        if (result) {
            ERR("bbitsDodDodGetOffsetCount should return false for empty%s",
                "");
        }

        bbitsDodXof dx = {};
        double *dvals = nullptr;

        result = bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &dvals,
                                           nullptr, nullptr, nullptr);
        if (result) {
            ERR("bbitsDodXofGetOffsetCount should return false for empty%s",
                "");
        }
    }

    /* Test offset reading */
    TEST("offset-based reading") {
        bbitsDodDod dd = {};
        const size_t numElements = 50;

        /* Write data */
        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)(i * 10), (dodVal)(i + 100));
        }

        /* Read with offset */
        ssize_t count = 10;
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;

        bool result = bbitsDodDodGetOffsetCount(&dd, 20, &count, &keys, &vals,
                                                nullptr, nullptr, nullptr);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount with offset failed");
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
    TEST("free on empty structures") {
        bbitsDodDod dd = {};
        bbitsDodDodFree(&dd); /* Should not crash */

        bbitsDodXof dx = {};
        bbitsDodXofFree(&dx); /* Should not crash */
    }

    /* Test negative offset (from end) */
    TEST("a negative offset (from the end)") {
        bbitsDodDod dd = {};
        const size_t numElements = 100;

        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)(i * 5), (dodVal)(i * 7));
        }

        /* Read last 10 elements using negative offset */
        ssize_t count = 10;
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;

        bool result = bbitsDodDodGetOffsetCount(&dd, -10, &count, &keys, &vals,
                                                nullptr, nullptr, nullptr);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount with negative offset failed");
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
    TEST("statistics calculation") {
        bbitsDodDod dd = {};

        /* Write known values: 10, 20, 30, 40, 50 - mean should be 30 */
        bbitsDodDodAppend(&dd, 1, 10);
        bbitsDodDodAppend(&dd, 2, 20);
        bbitsDodDodAppend(&dd, 3, 30);
        bbitsDodDodAppend(&dd, 4, 40);
        bbitsDodDodAppend(&dd, 5, 50);

        ssize_t count = -1;
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;
        double mean, variance, stddev;

        bool result = bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals,
                                                &mean, &variance, &stddev);
        if (!result) {
            ERR("bbitsDodDodGetOffsetCount for stats failed");
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
    TEST("offset/count range clamping") {
        /* Irregular values so the DoD stream spills into several segments */
#define BBITS_CLAMP_VAL(i) (((uint64_t)(i) * 2654435761u) % 1000003)
        const size_t numElements = 5000;
        bbitsDodDod dd = {};
        bbitsDodXof dx = {};
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
        for (size_t p = 0; p < countof(pastEnd); p++) {
            ssize_t count = 1;
            uint64_t *keys = nullptr;
            uint64_t *vals = nullptr;
            double *dvals = nullptr;
            if (bbitsDodDodGetOffsetCount(&dd, pastEnd[p], &count, &keys, &vals,
                                          nullptr, nullptr, nullptr)) {
                ERR("DodDod offset %zd should be refused", pastEnd[p]);
                zfree(keys);
                zfree(vals);
            }

            count = 1;
            if (bbitsDodXofGetOffsetCount(&dx, pastEnd[p], &count, &keys,
                                          &dvals, nullptr, nullptr, nullptr)) {
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
        for (size_t c = 0; c < countof(cases); c++) {
            const size_t start = cases[c][0] < 0
                                     ? numElements + (size_t)cases[c][0]
                                     : (size_t)cases[c][0];
            const size_t want = (size_t)cases[c][2];

            ssize_t count = cases[c][1];
            uint64_t *keys = nullptr;
            uint64_t *vals = nullptr;
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
            double *dvals = nullptr;
            if (!bbitsDodXofGetOffsetCount(&dx, cases[c][0], &count, &keys,
                                           &dvals, nullptr, nullptr, nullptr)) {
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
    TEST("statistics against a two-pass oracle") {
#define BBITS_STAT_DOD(i) (((uint64_t)(i) * 2654435761u) % 1000003)
#define BBITS_STAT_XOF(i) ((double)(i) * 0.123 - 77.0)
        const size_t numElements = 3000;
        bbitsDodDod dd = {};
        bbitsDodXof dx = {};
        for (size_t i = 0; i < numElements; i++) {
            bbitsDodDodAppend(&dd, (dodVal)i, (dodVal)BBITS_STAT_DOD(i));
            bbitsDodXofAppend(&dx, (dodVal)i, BBITS_STAT_XOF(i));
        }

        /* {offset, count}: whole set, a middle window, one element */
        const ssize_t windows[][2] = {{0, -1}, {700, 1500}, {2999, 1}};
        for (size_t w = 0; w < countof(windows); w++) {
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
                uint64_t *keys = nullptr;
                uint64_t *vals = nullptr;
                double *dvals = nullptr;
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
        uint64_t *keys = nullptr;
        uint64_t *vals = nullptr;
        double *dvals = nullptr;
        double mean = -1, variance = -1, stddev = -1;
        if (bbitsDodDodGetOffsetCount(&dd, 0, &count, &keys, &vals, &mean,
                                      &variance, &stddev) ||
            bbitsDodXofGetOffsetCount(&dx, 0, &count, &keys, &dvals, &mean,
                                      &variance, &stddev) ||
            keys || vals || dvals) {
            ERR("A zero count should be refused without allocating");
        }

        bbitsDodDodFree(&dd);
        bbitsDodXofFree(&dx);
#undef BBITS_STAT_DOD
#undef BBITS_STAT_XOF
    }

    TEST_FINAL_RESULT;
}
#endif
