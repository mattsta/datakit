#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t xof;
typedef uint64_t xofVal;

typedef struct xofWriter {
    xof *d;

    /* State of tail entry needed for appending next entry... */
    /* Next write position for 'd' */
    size_t usedBits;
    int_fast32_t currentLeadingZeroes;
    int_fast32_t currentTrailingZeroes;
    double prevVal;

    /* Number of elements inside 'd' */
    size_t count;

    /* Total bytes allocated for 'd' */
    size_t totalBytes;
} xofWriter;

/* ====================================================================
 * Resumable Reader - O(1) sequential access
 * Pass xof pointer on each call (safe for reallocation)
 * ==================================================================== */
typedef struct xofReader {
    size_t bitOffset;                   /* Bit position in stream */
    uint64_t currentValueBits;          /* Last decoded value as bits */
    uint_fast32_t currentLeadingZeroes; /* XOR block metadata */
    uint_fast32_t currentLengthOfBits;  /* XOR block length */
    size_t valuesRead;                  /* Count of values decoded */
} xofReader;

void xofInit(xof *x, size_t *bitsUsed, double val);
void xofAppend(xof *x, size_t *bitsUsed, int_fast32_t *prevLeadingZeroes,
               int_fast32_t *prevTrailingZeroes, double prevVal, double newVal);

double xofGet(const xof *x, size_t offset);
double xofGetCached(const xof *x, size_t *bitOffset, uint64_t *currentValueBits,
                    uint_fast32_t *currentLeadingZeroes,
                    uint_fast32_t *currentLengthOfBits, size_t offset);
bool xofReadAll(const xof *d, double *vals, size_t count);
void xofWrite(xofWriter *const w, const double val);

/* ====================================================================
 * xofReader API - O(1) resumable sequential access
 * ==================================================================== */

/* Initialize reader from raw xof data */
void xofReaderInit(xofReader *r, const xof *x);

/* Initialize reader from a writer */
void xofReaderInitFromWriter(xofReader *r, const xofWriter *w);

/* Read next value - O(1) operation. Advances iterator state. */
double xofReaderNext(xofReader *r, const xof *x);

/* Peek at current value without advancing */
double xofReaderCurrent(const xofReader *r);

/* Batch read - reads up to n values into out[], returns count read */
size_t xofReaderNextN(xofReader *r, const xof *x, double *out, size_t n);

/* Get count of values remaining (requires knowing total count) */
size_t xofReaderRemaining(const xofReader *r, size_t totalCount);

#ifdef DATAKIT_TEST
int xofTest(int argc, char *argv[]);
#endif
