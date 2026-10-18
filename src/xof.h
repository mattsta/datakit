#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t xof;
typedef uint64_t xofVal;

/* Writer: xofWrite() appends values to 'd'. Start from {0} (every field
 * zero); the writer resets its XOR window on the first value. */
typedef struct xofWriter {
    /* Stream, owned by the writer: xofWrite() allocates it when NULL and
     * grows it with zrealloc as values arrive, keeping 'totalBytes' current
     * and room for readers to decode everything written. A caller may
     * preallocate it instead, zero-filled from the zmalloc family, with
     * 'totalBytes' set to its size. Free it with zfree(). */
    xof *d;

    /* State of tail entry needed for appending next entry... */
    /* Next write position for 'd' */
    size_t usedBits;
    int_fast32_t currentLeadingZeroes;
    int_fast32_t currentTrailingZeroes;
    double prevVal;

    /* Number of elements inside 'd' */
    size_t count;

    /* Bytes allocated for 'd' (ignored while 'd' is NULL) */
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

/* Raw stream: xofInit() writes the first value, then each xofAppend()
 * encodes the next one against the previous. '*prevLeadingZeroes' and
 * '*prevTrailingZeroes' carry the XOR window between calls and must start
 * at -1, so the first XOR entry sets a window before any entry reuses it
 * (xofWrite() does this itself). */
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
