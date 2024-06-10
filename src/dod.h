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

dodVal dodGet(const dod *d, size_t *consumedBits, dodVal originalStartVal,
              dodVal currentVal, size_t valueOffsetToReturn);
void dodAppend(dod *d, dodVal t0, dodVal t1, dodVal newVal,
               size_t *currentBits);

dodVal dodRead(dodWriter *const w, const size_t offset);
void dodWrite(dodWriter *const w, const dodVal val);
void dodInitFromExisting(dodWriter *const w, const dod *d);
void dodCloseWrites(dodWriter *const w);

bool dodReadAll(const dod *w, uint64_t *vals, size_t count);

#ifdef DATAKIT_TEST
int dodTest(int argc, char *argv[]);
#endif
