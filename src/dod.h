#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint64_t dod;
typedef int64_t dodVal;

typedef struct dodWriter {
    dod *d;

    /* If this writer is OPEN, then t[2] is the write helpers.
     * If this writer is CLOSED, then t[2] is:
     *  - [0] - lowest complete value in 'd' (first)
     *  - [1] - highest complete value in 'd' (last) */
    dodVal t[2];

    /* Number of elements in 'd' */
    /* (If <= 2, values are ONLY in t[2] */
    size_t count;

    /* Next bit offset to write inside 'd' */
    size_t usedBits;

    /* Total bytes allocated for 'd' */
    size_t totalBytes;
} dodWriter;

/* ====================================================================
 * Resumable Reader - O(1) sequential access
 * ==================================================================== */
typedef struct dodReader {
    size_t consumedBits; /* Bit position in stream */
    dodVal t0;           /* Second-to-last value (for delta-of-delta) */
    dodVal t1;           /* Last decoded value */
    size_t valuesRead;   /* Count of values decoded so far */
} dodReader;

dodVal dodGet(const dod *d, size_t *consumedBits, dodVal originalStartVal,
              dodVal currentVal, size_t valueOffsetToReturn);
void dodAppend(dod *d, dodVal t0, dodVal t1, dodVal newVal,
               size_t *currentBits);

dodVal dodRead(dodWriter *const w, const size_t offset);
void dodWrite(dodWriter *const w, const dodVal val);
void dodInitFromExisting(dodWriter *const w, const dod *d);
void dodCloseWrites(dodWriter *const w);

bool dodReadAll(const dod *w, uint64_t *vals, size_t count);

/* ====================================================================
 * dodReader API - O(1) resumable sequential access
 * ==================================================================== */

/* Initialize reader from first two values (t0, t1) */
void dodReaderInit(dodReader *r, dodVal firstVal, dodVal secondVal);

/* Initialize reader from a writer (reads first two values from writer state) */
void dodReaderInitFromWriter(dodReader *r, const dodWriter *w);

/* Read next value - O(1) operation. Advances iterator state. */
dodVal dodReaderNext(dodReader *r, const dod *d);

/* Peek at current value without advancing (returns t1) */
dodVal dodReaderCurrent(const dodReader *r);

/* Batch read - reads up to n values into out[], returns count read */
size_t dodReaderNextN(dodReader *r, const dod *d, dodVal *out, size_t n);

/* Get count of values remaining (requires knowing total count) */
size_t dodReaderRemaining(const dodReader *r, size_t totalCount);

#ifdef DATAKIT_TEST
int dodTest(int argc, char *argv[]);
#endif
