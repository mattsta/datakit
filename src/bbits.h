#pragma once

#include "dod.h"
#include "xof.h"

#include <unistd.h> /* ssize_t */

/* Paired time series: a delta-of-delta key stream (timestamps, ids)
 * with a value stream beside it, delta-of-delta for integers
 * (bbitsDodDod) or xor-of-float for doubles (bbitsDodXof).  A
 * zero-initialised struct is empty; Append grows segments as needed;
 * Free releases them. */

/* DoD needs two initial values to start writing. */
typedef struct bbitsDodDod {
    /* Arrays of key and value writers.  Each writer keeps its segment's
     * first two values itself (t[0], then the stream header). */
    dodWriter *key;
    dodWriter *val;

    /* Length of both 'key' and 'val' arrays. */
    size_t count;

    /* Count of all elements inside 'key' (which must be same count as 'val') */
    size_t elements;
} bbitsDodDod;

/* XoF only needs one value to start writing. */
typedef struct bbitsDodXof {
    /* Arrays of key and value writers.  Each key writer keeps its segment's
     * first two values itself (t[0], then the stream header). */
    dodWriter *key;
    xofWriter *val;

    /* Length of both 'key' and 'val' arrays. */
    size_t count;

    /* Count of all elements inside 'key' (which must be same count as 'val') */
    size_t elements;
} bbitsDodXof;

/* ====================================================================
 * Dod - Dod
 * ==================================================================== */
bool bbitsDodDodAppend(bbitsDodDod *dd, const dodVal newKey,
                       const dodVal newVal);

/* Read '*count' pairs starting at 'offset' into new zmalloc'd '*key' and
 * '*val' arrays (caller frees both with zfree). A negative 'offset' counts
 * back from the end. A negative '*count', or one running past the end, reads
 * to the end, and '*count' is set to the number of pairs actually read.
 * Returns false, allocating nothing, when there is nothing to read: the
 * structure is empty, 'offset' lies outside it, or '*count' is 0. If 'mean',
 * 'variance' and 'stddev' are all non-NULL they receive the population
 * statistics of the values read (variance = sum of squared deviations / n,
 * stddev = its square root); n is at least 1 whenever true is returned. The
 * DodXof variant below follows the same contract. */
bool bbitsDodDodGetOffsetCount(bbitsDodDod *dd, ssize_t offset, ssize_t *count,
                               uint64_t **key, uint64_t **val, double *mean,
                               double *variance, double *stddev);
void bbitsDodDodFree(bbitsDodDod *dd);

/* ====================================================================
 * Dod - Xof
 * ==================================================================== */
bool bbitsDodXofAppend(bbitsDodXof *dx, const dodVal newKey,
                       const double newVal);
bool bbitsDodXofGetOffsetCount(bbitsDodXof *dx, ssize_t offset, ssize_t *count,
                               uint64_t **key, double **val, double *mean,
                               double *variance, double *stddev);
void bbitsDodXofFree(bbitsDodXof *dx);

#ifdef DATAKIT_TEST
int bbitsTest(int argc, char *argv[]);
#endif
