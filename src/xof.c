#include "xof.h"

#include <assert.h>
#include <string.h> /* memcpy */

#include "datakit.h"

/* ====================================================================
 * Give varintBitstream our types for the bitstream
 * ==================================================================== */
#define VBITS xof
#define VBITSVAL xofVal
#include "../deps/varint/src/varintBitstream.h"

/* ====================================================================
 * Types
 * ==================================================================== */
typedef enum xofType {
    XOF_TYPE_SAME = 0x02, /* 00000010 */
    XOF_TYPE_NEW = 0x03   /* 00000011 */
} xofType;

#define BITS_TYPE 2
#define BITS_LEADING_ZEROS 6
#define BITS_DATA 6

/* ====================================================================
 * Create
 * ==================================================================== */
void xofInit(xof *restrict const x, size_t *restrict const bitsUsed,
             const double val) {
    assert(sizeof(uint64_t) == sizeof(double));

    const uint64_t *const v = (uint64_t *)&val;
    varintBitstreamSet(x, 0, 64, *v);
    *bitsUsed = 64;
}

/* ====================================================================
 * Append new value
 * ==================================================================== */
void xofAppend(xof *restrict const x, size_t *restrict const bitsUsed,
               int_fast32_t *restrict const prevLeadingZeroes,
               int_fast32_t *restrict const prevTrailingZeroes,
               const double prevVal, const double newVal) {
/* Get binary representation of the doubles */
#if XOF_DIRECT_ACCESS
    /* -fno-strict-aliasing */
    const uint64_t o = *(uint64_t *)&prevVal;
    const uint64_t v = *(uint64_t *)&newVal;
#else
    /* -fstrict-aliasing */
    uint64_t o = 0;
    uint64_t v = 0;
    memcpy(&o, &prevVal, sizeof(prevVal));
    memcpy(&v, &newVal, sizeof(newVal));
#endif

    /* ULL because of builtin APIs */
    const unsigned long long compared = o ^ v;
    if (compared == 0) {
        /* use single zero bit, no change in data. */
        *bitsUsed += 1;
        return;
    }

    /* Data layout:
     *   META: 10 (SAME)
     *   DATA: block position same as previous, just store unique values.
     *
     *   META: 11 (NEW)
     *   DATA:
     *     - 6 bits for length of leading zeroes (2^6 == 64)
     *     - 6 bits for length of unique value (2^6 == 64)
     *     - unique value */

    /* drop trailing zeroes to get the unique */
    /* We don't need a mask because the layout is:
     *   [LEADING ZEROES][DATA][TRAILING ZEROES]
     * so, we know we have *only* leading zeros above data bits. */

    /* int because of builtin APIs */
    const int newLeadingZeroes = __builtin_clzll(compared);
    const int newTrailingZeroes = __builtin_ctzll(compared);

    const int32_t lengthOfOldBits =
        64 - *prevLeadingZeroes - *prevTrailingZeroes;
    const int32_t lengthOfNewBits = 64 - newLeadingZeroes - newTrailingZeroes;

    if (((*prevLeadingZeroes >= newLeadingZeroes) &&
         (*prevTrailingZeroes >= newTrailingZeroes)) &&
        (lengthOfOldBits >= lengthOfNewBits)) {
        /* this xor data fits in the same range as
         * the previous encoding, but it *may* be smaller, so we
         * must force encode using the *previous* data range
         * instead of the exact data range for this xor result */
        const uint64_t bits = compared >> *prevTrailingZeroes;

        varintBitstreamSet(x, *bitsUsed, BITS_TYPE, XOF_TYPE_SAME);
        *bitsUsed += BITS_TYPE;

        /* bits >> 64 is undefined behavior, so guard against it */
        assert(lengthOfOldBits >= 64 || (bits >> lengthOfOldBits) == 0);

        varintBitstreamSet(x, *bitsUsed, lengthOfOldBits, bits);
        *bitsUsed += lengthOfOldBits;
    } else {
        /* else, need to specify a new range */
        const uint64_t bits = compared >> newTrailingZeroes;

        varintBitstreamSet(x, *bitsUsed, BITS_TYPE, XOF_TYPE_NEW);
        *bitsUsed += BITS_TYPE;

        varintBitstreamSet(x, *bitsUsed, BITS_LEADING_ZEROS, newLeadingZeroes);
        *bitsUsed += BITS_LEADING_ZEROS;

        /* lengthOfNewBits is 1-64, but BITS_DATA (6 bits) can only store 0-63.
         * Store length-1 and add 1 when reading. */
        assert(lengthOfNewBits > 0 && lengthOfNewBits <= 64);
        varintBitstreamSet(x, *bitsUsed, BITS_DATA, lengthOfNewBits - 1);
        *bitsUsed += BITS_DATA;

        varintBitstreamSet(x, *bitsUsed, lengthOfNewBits, bits);
        *bitsUsed += lengthOfNewBits;

        *prevLeadingZeroes = newLeadingZeroes;
        *prevTrailingZeroes = newTrailingZeroes;
    }
}

/* ====================================================================
 * Get value at offset
 * ==================================================================== */
double xofGetCached(const xof *restrict const x,
                    size_t *restrict const bitOffset,
                    uint64_t *restrict const currentValueBits,
                    uint_fast32_t *restrict const currentLeadingZeroes,
                    uint_fast32_t *restrict const currentLengthOfBits,
                    size_t offset) {
    static const void *lookups[4] = {NULL, NULL, &&Same, &&New};
    uint_fast32_t consumedValues = 0;
    uint64_t unique = 0;

Dispatch:
    if (consumedValues == offset) {
        return *(double *)currentValueBits;
    }

    consumedValues++;

    const uint8_t grabBits = varintBitstreamGet(x, *bitOffset, BITS_TYPE);
    if (grabBits < 2) {
        /* zero == current value is previous value, nothing to do */
        /* just eat one bit then continue; values don't change. */
        *bitOffset += 1;
        goto Dispatch;
    }

    goto *lookups[grabBits];

Same:
    /* same == same leading/trailing offset as before, just reconstitute. */
    *bitOffset += BITS_TYPE; /* jump over type bits */

    unique = varintBitstreamGet(x, *bitOffset, *currentLengthOfBits);
    *bitOffset += *currentLengthOfBits;

    /* Restore lower and upper leading zeroes */
    unique <<= (64 - *currentLeadingZeroes - *currentLengthOfBits);

    /* recompute current value */
    *currentValueBits ^= unique;

    goto Dispatch;

New:
    /* new == new leading offset, reconstitute as new */
    *bitOffset += BITS_TYPE; /* jump over type bits */

    *currentLeadingZeroes =
        varintBitstreamGet(x, *bitOffset, BITS_LEADING_ZEROS);
    *bitOffset += BITS_LEADING_ZEROS;

    /* Length stored as length-1 (0-63), so add 1 to get actual (1-64) */
    *currentLengthOfBits = varintBitstreamGet(x, *bitOffset, BITS_DATA) + 1;
    *bitOffset += BITS_DATA;

    unique = varintBitstreamGet(x, *bitOffset, *currentLengthOfBits);
    *bitOffset += *currentLengthOfBits;

    /* Restore lower and upper leading zeroes */
    unique <<= (64 - *currentLeadingZeroes - *currentLengthOfBits);

    /* recompute current value */
    *currentValueBits ^= unique;

    goto Dispatch;

    __builtin_unreachable();
}

double xofGet(const xof *const x, const size_t offset) {
    uint_fast32_t currentLeadingZeroes = 0;
    uint_fast32_t currentLengthOfBits = 0;

    uint64_t currentValueBits = varintBitstreamGet(x, 0, 64);
    size_t bitOffset = 64;

    return xofGetCached(x, &bitOffset, &currentValueBits, &currentLeadingZeroes,
                        &currentLengthOfBits, offset);
}

void xofWrite(xofWriter *const w, const double val) {
    if (w->count == 0) {
        /* First value: use xofInit to write raw 64-bit double */
        xofInit(w->d, &w->usedBits, val);
    } else {
        /* Subsequent values: encode as XOR delta from previous */
        xofAppend(w->d, &w->usedBits, &w->currentLeadingZeroes,
                  &w->currentTrailingZeroes, w->prevVal, val);
    }

    w->prevVal = val;
    w->count++;
}

bool xofReadAll(const xof *x, double *vals, size_t count) {
    if (!count) {
        return false;
    }

    uint_fast32_t currentLeadingZeroes = 0;
    uint_fast32_t currentLengthOfBits = 0;

    /* Read first full length entry... */
    uint64_t currentValueBits = varintBitstreamGet(x, 0, 64);
    size_t bitOffset = 64;

    /* Write first entry (bit reinterpret, not numeric cast)... */
    vals[0] = *(double *)&currentValueBits;

    /* Fetch remaining entries... */
    for (size_t i = 1; i < count; i++) {
        vals[i] = xofGetCached(x, &bitOffset, &currentValueBits,
                               &currentLeadingZeroes, &currentLengthOfBits, 1);
    }

    return true;
}
/* ====================================================================
 * xofReader API - O(1) resumable sequential access
 * ==================================================================== */

void xofReaderInit(xofReader *r, const xof *x) {
    /* Read the first 64-bit value directly */
    r->currentValueBits = varintBitstreamGet(x, 0, 64);
    r->bitOffset = 64;
    r->currentLeadingZeroes = 0;
    r->currentLengthOfBits = 0;
    r->valuesRead = 1;
}

void xofReaderInitFromWriter(xofReader *r, const xofWriter *w) {
    if (w->count == 0) {
        /* Empty writer - initialize to empty state */
        r->bitOffset = 0;
        r->currentValueBits = 0;
        r->currentLeadingZeroes = 0;
        r->currentLengthOfBits = 0;
        r->valuesRead = 0;
        return;
    }

    /* Initialize from writer's data */
    xofReaderInit(r, w->d);
}

double xofReaderNext(xofReader *r, const xof *x) {
    /* Use xofGetCached to read exactly 1 value and advance state */
    double val =
        xofGetCached(x, &r->bitOffset, &r->currentValueBits,
                     &r->currentLeadingZeroes, &r->currentLengthOfBits, 1);
    r->valuesRead++;
    return val;
}

double xofReaderCurrent(const xofReader *r) {
    /* Return current value without advancing */
    return *(double *)&r->currentValueBits;
}

size_t xofReaderNextN(xofReader *r, const xof *x, double *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] =
            xofGetCached(x, &r->bitOffset, &r->currentValueBits,
                         &r->currentLeadingZeroes, &r->currentLengthOfBits, 1);
        r->valuesRead++;
    }

    return n;
}

size_t xofReaderRemaining(const xofReader *r, size_t totalCount) {
    if (r->valuesRead >= totalCount) {
        return 0;
    }

    return totalCount - r->valuesRead;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#define RD(max) (((double)rand() / (double)(RAND_MAX)) * (max))
int xofTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const size_t loopers = 10000;

    setlocale(LC_ALL, "");

    for (int32_t val = -1; val < 2; val++) {
        TEST_DESC("all same (%d)", val) {
            xof *bits = zcalloc(loopers, sizeof(*bits));
            TEST("encode") {
                PERF_TIMERS_SETUP;
                size_t bitsUsed = 0;
                xofInit(bits, &bitsUsed, val);

                int_fast32_t prevLZ = -1;
                int_fast32_t prevTZ = -1;
                for (size_t i = 2; i < loopers; i++) {
                    xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, val, val);
                }

                printf(
                    "Simple encode of %'zu same integers used %'zu bits (%0.2f "
                    "bytes); "
                    "avg %0.2f bits per entry!\n",
                    loopers, bitsUsed, (double)bitsUsed / 8,
                    (double)bitsUsed / loopers);
                PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                                 "encode with cached results");
            }

            TEST("decode last (n)") {
                PERF_TIMERS_SETUP;
                double got = xofGet(bits, loopers - 1);
                double expected = val;
                if (got != expected) {
                    ERR("Expected %f but got %f instead!\n", expected, got);
                    assert(NULL);
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
            }

            TEST("decode all (n^2)") {
                PERF_TIMERS_SETUP;
                for (size_t i = 0; i < loopers; i++) {
                    double got = xofGet(bits, i);
                    double expected = val;
                    if (got != expected) {
                        ERR("[%'zu] Expected %f but got %f instead!\n", i,
                            expected, got);
                    }
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(
                    loopers, "decode from beginning per lookup");
            }
            zfree(bits);
            printf("\n");
        }
    }

    for (int32_t val = -7; val <= 7; val++) {
        TEST_DESC("alternating (%d, %d)", val, val + 1) {
            xof *bits = zcalloc(loopers, sizeof(*bits));
            double mmax = 0;
            TEST("encode") {
                PERF_TIMERS_SETUP;
                size_t bitsUsed = 0;
                xofInit(bits, &bitsUsed, val);

                int_fast32_t prevLZ = -1;
                int_fast32_t prevTZ = -1;
                double prevVal = val;
                for (size_t i = 2; i < loopers + 1; i++) {
                    double currentVal = i % 2 == 0 ? val + 1 : val;
                    xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, prevVal,
                              currentVal);
                    prevVal = currentVal;
                }
                mmax = prevVal;

                printf(
                    "Simple encode of %'zu same integers used %'zu bits (%0.2f "
                    "bytes); "
                    "avg %0.2f bits per entry!\n",
                    loopers, bitsUsed, (double)bitsUsed / 8,
                    (double)bitsUsed / loopers);
                PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                                 "encode with cached results");
            }

            TEST("decode last (n)") {
                PERF_TIMERS_SETUP;
                double got = xofGet(bits, loopers - 1);
                double expected = mmax;
                if (got != expected) {
                    ERR("Expected %f but got %f instead!\n", expected, got);
                    assert(NULL);
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
            }

            TEST("decode all (n^2)") {
                PERF_TIMERS_SETUP;
                for (size_t i = 0; i < loopers; i++) {
                    double got = xofGet(bits, i);
                    double expected = i % 2 == 0 ? val : val + 1;
                    if (got != expected) {
                        ERR("[%'zu] Expected %f but got %f instead!\n", i,
                            expected, got);
                    }
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(
                    loopers, "decode from beginning per lookup");
            }
            zfree(bits);
            printf("\n");
        }
    }

    TEST("simples") {
        xof *bits = zcalloc(loopers, sizeof(*bits));
        TEST("encode") {
            PERF_TIMERS_SETUP;
            size_t bitsUsed = 0;
            xofInit(bits, &bitsUsed, 1);

            double prevVal = 1;
            int_fast32_t prevLZ = -1;
            int_fast32_t prevTZ = -1;
            for (size_t i = 2; i < loopers; i++) {
                xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, prevVal, i);
                prevVal = i;
            }

            printf("Simple encode of %'zu sequential integers used %'zu bits "
                   "(%0.2f bytes); "
                   "avg %0.2f bits per entry!\n",
                   loopers, bitsUsed, (double)bitsUsed / 8,
                   (double)bitsUsed / loopers);
            PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                             "encode with cached results");
        }

        TEST("decode last (n)") {
            PERF_TIMERS_SETUP;
            double got = xofGet(bits, loopers - 1);
            double expected = loopers - 1;
            if (got != expected) {
                ERR("Expected %f but got %f instead!\n", expected, got);
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
        }

        TEST("decode all (n^2)") {
            PERF_TIMERS_SETUP;
            for (size_t i = 1; i < loopers; i++) {
                double got = xofGet(bits, i - 1);
                double expected = i;
                if (got != expected) {
                    ERR("[%'zu] Expected %f but got %f instead!\n", i, expected,
                        got);
                }
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(
                loopers, "decode from beginning per lookup");
        }
        zfree(bits);
        printf("\n");
    }

    TEST("random") {
        xof *bits = zcalloc(loopers * 2, sizeof(*bits));
        double *values = zcalloc(loopers, sizeof(*values));
        TEST("encode") {
            for (size_t i = 0; i < loopers; i++) {
                values[i] = RD(UINT16_MAX);
            }

            PERF_TIMERS_SETUP;

            size_t bitsUsed = 0;
            xofInit(bits, &bitsUsed, values[0]);

            int_fast32_t prevLZ = -1;
            int_fast32_t prevTZ = -1;
            for (size_t i = 1; i < loopers; i++) {
                xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                          values[i]);
            }

            printf(
                "Random encode of %'zu doubles used %'zu bits (%0.2f bytes); "
                "avg %0.2f bits per entry!\n",
                loopers, bitsUsed, (double)bitsUsed / 8,
                (double)bitsUsed / loopers);
            PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                             "encode with cached results");
        }

        TEST("decode last (n)") {
            PERF_TIMERS_SETUP;
            double got = xofGet(bits, loopers - 1);
            double expected = values[loopers - 1];
            if (got != expected) {
                ERR("Expected %f but got %f instead!\n", expected, got);
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
        }

        TEST("decode all (n^2)") {
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < loopers; i++) {
                double got = xofGet(bits, i);
                double expected = values[i];
                if (got != expected) {
                    ERR("[%'zu] Expected %f but got %f instead!\n", i, expected,
                        got);
                    assert(NULL);
                }
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(
                loopers, "decode from beginning per lookup");
        }
        zfree(bits);
        printf("\n");
    }

    double deltas[] = {0.77,   0.077,   0.0077, 0.00077, 0.33,  0.033,
                       0.0033, 0.00033, 1,      2,       0.0004};
    for (size_t q = 0; q < sizeof(deltas) / sizeof(*deltas); q++) {
        TEST_DESC("random (bounded random delta range; delta %f)", deltas[q]) {
            xof *bits = zcalloc(loopers * 2, sizeof(*bits));
            double *values = zcalloc(loopers, sizeof(*values));
            TEST("encode") {
                for (size_t i = 0; i < loopers; i++) {
                    values[i] = (double)1 + RD(deltas[q]);
                }

                PERF_TIMERS_SETUP;

                size_t bitsUsed = 0;
                xofInit(bits, &bitsUsed, values[0]);

                int_fast32_t prevLZ = -1;
                int_fast32_t prevTZ = -1;
                for (size_t i = 1; i < loopers; i++) {
                    xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                              values[i]);
                }

                printf("Random encode of %'zu doubles used %'zu bits (%0.2f "
                       "bytes); "
                       "avg %0.2f bits per entry!\n",
                       loopers, bitsUsed, (double)bitsUsed / 8,
                       (double)bitsUsed / loopers);
                PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                                 "encode with cached results");
            }

            TEST("decode last (n)") {
                PERF_TIMERS_SETUP;
                double got = xofGet(bits, loopers - 1);
                double expected = values[loopers - 1];
                if (got != expected) {
                    ERR("Expected %f but got %f instead!\n", expected, got);
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
            }

            TEST("decode all (n^2)") {
                PERF_TIMERS_SETUP;
                for (size_t i = 0; i < loopers; i++) {
                    double got = xofGet(bits, i);
                    double expected = values[i];
                    if (got != expected) {
                        ERR("[%'zu] Expected %f but got %f instead!\n", i,
                            expected, got);
                    }
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(
                    loopers, "decode from beginning per lookup");
            }
            zfree(bits);
            printf("\n");
        }
    }

    for (size_t q = 0; q < 30; q++) {
        TEST_DESC("random (unbounded random iteration %'zu)", q) {
            xof *bits = zcalloc(loopers * 2, sizeof(*bits));
            double *values = zcalloc(loopers, sizeof(*values));
            TEST("encode") {
                values[0] = RD(74);
                for (size_t i = 1; i < loopers; i++) {
                    if (rand() % ((rand() % 6) + 1) == 0) {
                        /* 1/7th of the time, generate a new additive value */
                        values[i] = values[i - 1] + RD(0.001) * q;
                    } else {
                        /* else, use previous value */
                        values[i] = values[i - 1];
                    }
                }

                PERF_TIMERS_SETUP;

                size_t bitsUsed = 0;
                xofInit(bits, &bitsUsed, values[0]);

                int_fast32_t prevLZ = -1;
                int_fast32_t prevTZ = -1;
                for (size_t i = 1; i < loopers; i++) {
                    xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                              values[i]);
                }

                printf("Random encode of %'zu doubles used %'zu bits (%0.2f "
                       "bytes); "
                       "avg %0.2f bits per entry!\n",
                       loopers, bitsUsed, (double)bitsUsed / 8,
                       (double)bitsUsed / loopers);
                PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                                 "encode with cached results");
            }

            TEST("decode last (n)") {
                PERF_TIMERS_SETUP;
                double got = xofGet(bits, loopers - 1);
                double expected = values[loopers - 1];
                if (got != expected) {
                    ERR("Expected %f but got %f instead!\n", expected, got);
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
            }

            TEST("decode all (n^2)") {
                PERF_TIMERS_SETUP;
                for (size_t i = 0; i < loopers; i++) {
                    double got = xofGet(bits, i);
                    double expected = values[i];
                    if (got != expected) {
                        ERR("[%'zu] Expected %f but got %f instead!\n", i,
                            expected, got);
                    }
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(
                    loopers, "decode from beginning per lookup");
            }
            zfree(bits);
            printf("\n");
        }
    }

    /* ================================================================
     * xofReader tests
     * ================================================================ */
    TEST("xofReader - sequential reading") {
        const size_t count = 10000;
        xof *bits = zcalloc(count * 2, sizeof(*bits));
        double *values = zcalloc(count, sizeof(*values));

        /* Generate test data */
        values[0] = 100.5;
        for (size_t i = 1; i < count; i++) {
            values[i] = values[i - 1] + RD(0.1);
        }

        /* Encode */
        size_t bitsUsed = 0;
        xofInit(bits, &bitsUsed, values[0]);
        int_fast32_t prevLZ = -1;
        int_fast32_t prevTZ = -1;
        for (size_t i = 1; i < count; i++) {
            xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                      values[i]);
        }

        /* Test xofReaderInit and sequential reading */
        xofReader r;
        xofReaderInit(&r, bits);

        /* First value should be available via xofReaderCurrent */
        double first = xofReaderCurrent(&r);
        if (first != values[0]) {
            ERR("xofReaderCurrent: expected %f but got %f\n", values[0], first);
        }

        /* Read remaining values sequentially */
        for (size_t i = 1; i < count; i++) {
            double got = xofReaderNext(&r, bits);
            if (got != values[i]) {
                ERR("[%zu] Expected %f but got %f\n", i, values[i], got);
            }
        }

        /* Verify valuesRead count */
        if (r.valuesRead != count) {
            ERR("Expected valuesRead=%zu but got %zu\n", count, r.valuesRead);
        }

        zfree(bits);
        zfree(values);
        printf("xofReader sequential reading: PASS\n");
    }

    TEST("xofReader - batch reading with NextN") {
        const size_t count = 10000;
        xof *bits = zcalloc(count * 2, sizeof(*bits));
        double *values = zcalloc(count, sizeof(*values));
        double *readBuf = zcalloc(count, sizeof(*readBuf));

        /* Generate test data */
        values[0] = 50.0;
        for (size_t i = 1; i < count; i++) {
            values[i] = values[i - 1] + RD(0.05);
        }

        /* Encode */
        size_t bitsUsed = 0;
        xofInit(bits, &bitsUsed, values[0]);
        int_fast32_t prevLZ = -1;
        int_fast32_t prevTZ = -1;
        for (size_t i = 1; i < count; i++) {
            xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                      values[i]);
        }

        /* Read with NextN in batches */
        xofReader r;
        xofReaderInit(&r, bits);

        /* First value already decoded */
        readBuf[0] = xofReaderCurrent(&r);

        /* Read rest in a batch */
        size_t read = xofReaderNextN(&r, bits, readBuf + 1, count - 1);
        if (read != count - 1) {
            ERR("xofReaderNextN returned %zu, expected %zu\n", read, count - 1);
        }

        /* Verify all values */
        for (size_t i = 0; i < count; i++) {
            if (readBuf[i] != values[i]) {
                ERR("[%zu] Expected %f but got %f\n", i, values[i], readBuf[i]);
            }
        }

        zfree(bits);
        zfree(values);
        zfree(readBuf);
        printf("xofReader batch reading: PASS\n");
    }

    TEST("xofReader - performance comparison O(N) vs O(N^2)") {
        const size_t count = 10000;
        xof *bits = zcalloc(count * 2, sizeof(*bits));
        double *values = zcalloc(count, sizeof(*values));

        /* Generate test data */
        values[0] = 1.0;
        for (size_t i = 1; i < count; i++) {
            values[i] = values[i - 1] + RD(0.01);
        }

        /* Encode */
        size_t bitsUsed = 0;
        xofInit(bits, &bitsUsed, values[0]);
        int_fast32_t prevLZ = -1;
        int_fast32_t prevTZ = -1;
        for (size_t i = 1; i < count; i++) {
            xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                      values[i]);
        }

        /* O(N) sequential with xofReader */
        PERF_TIMERS_SETUP;
        xofReader r;
        xofReaderInit(&r, bits);
        double first = xofReaderCurrent(&r);
        if (first != values[0]) {
            ERRR("First value mismatch!");
        }

        for (size_t i = 1; i < count; i++) {
            double got = xofReaderNext(&r, bits);
            if (got != values[i]) {
                ERR("[%zu] Expected %f but got %f\n", i, values[i], got);
            }
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(count, "O(N) xofReader sequential");

        /* O(N^2) naive with xofGet */
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < count; i++) {
            double got = xofGet(bits, i);
            if (got != values[i]) {
                ERR("[%zu] Expected %f but got %f\n", i, values[i], got);
            }
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(count, "O(N^2) xofGet from beginning");

        zfree(bits);
        zfree(values);
        printf("xofReader performance comparison: PASS\n");
    }

    TEST("xofReader - xofReaderInitFromWriter") {
        xofWriter w = {0};
        w.d = zcalloc(1000, sizeof(*w.d));
        w.totalBytes = 1000 * sizeof(*w.d);

        /* Write some values */
        double values[] = {100.0, 100.5, 101.0, 101.5, 102.0};
        size_t count = sizeof(values) / sizeof(*values);
        for (size_t i = 0; i < count; i++) {
            xofWrite(&w, values[i]);
        }

        /* Initialize reader from writer */
        xofReader r;
        xofReaderInitFromWriter(&r, &w);

        /* First value */
        double first = xofReaderCurrent(&r);
        if (first != values[0]) {
            ERR("First value: expected %f but got %f\n", values[0], first);
        }

        /* Read remaining */
        for (size_t i = 1; i < count; i++) {
            double got = xofReaderNext(&r, w.d);
            if (got != values[i]) {
                ERR("[%zu] Expected %f but got %f\n", i, values[i], got);
            }
        }

        /* Test remaining count */
        size_t remaining = xofReaderRemaining(&r, w.count);
        if (remaining != 0) {
            ERR("Expected 0 remaining but got %zu\n", remaining);
        }

        zfree(w.d);
        printf("xofReaderInitFromWriter: PASS\n");
    }

    TEST("xofReader - xofReaderRemaining") {
        xofWriter w = {0};
        w.d = zcalloc(1000, sizeof(*w.d));
        w.totalBytes = 1000 * sizeof(*w.d);

        /* Write 10 values */
        for (int i = 0; i < 10; i++) {
            xofWrite(&w, (double)i * 1.5);
        }

        xofReader r;
        xofReaderInitFromWriter(&r, &w);

        /* After init, 1 value read (first), 9 remaining */
        if (xofReaderRemaining(&r, w.count) != 9) {
            ERR("Expected 9 remaining, got %zu\n",
                xofReaderRemaining(&r, w.count));
        }

        /* Read 4 more values */
        for (int i = 0; i < 4; i++) {
            xofReaderNext(&r, w.d);
        }

        /* Now 5 values read, 5 remaining */
        if (xofReaderRemaining(&r, w.count) != 5) {
            ERR("Expected 5 remaining, got %zu\n",
                xofReaderRemaining(&r, w.count));
        }

        zfree(w.d);
        printf("xofReaderRemaining: PASS\n");
    }

    TEST_FINAL_RESULT;
}
#endif
