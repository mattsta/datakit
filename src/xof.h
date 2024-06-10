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

typedef struct xofReader {
    xof *d;
    size_t bitOffset;
    uint64_t currentValueBits;
    uint_fast32_t currentLeadingZeroes;
    uint_fast32_t currentLengthOfBits;
    size_t offset;
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

#ifdef DATAKIT_TEST
int xofTest(int argc, char *argv[]);
#endif
