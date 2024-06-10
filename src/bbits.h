#include "dod.h"
#include "xof.h"

#include <unistd.h> /* ssize_t */

/* DoD needs two initial values to start writing. */
typedef struct bbitsDodDod {
    /* Arrays of key and value writers */
    dodWriter *key;
    dodWriter *val;

    /* Length of both 'key' and 'val' arrays. */
    size_t count;

    /* Count of all elements inside 'key' (which must be same count as 'val') */
    size_t elements;
} bbitsDodDod;

/* XoF only needs one value to start writing. */
typedef struct bbitsDodXof {
    /* Arrays of key and value writers */
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
