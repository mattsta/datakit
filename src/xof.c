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

        assert((bits >> lengthOfOldBits) == 0);

        varintBitstreamSet(x, *bitsUsed, lengthOfOldBits, bits);
        *bitsUsed += lengthOfOldBits;
    } else {
        /* else, need to specify a new range */
        const uint64_t bits = compared >> newTrailingZeroes;

        assert(lengthOfNewBits == (64 - __builtin_clzll(bits)));
        assert((bits >> lengthOfNewBits) == 0);

        varintBitstreamSet(x, *bitsUsed, BITS_TYPE, XOF_TYPE_NEW);
        *bitsUsed += BITS_TYPE;

        varintBitstreamSet(x, *bitsUsed, BITS_LEADING_ZEROS, newLeadingZeroes);
        *bitsUsed += BITS_LEADING_ZEROS;

        assert(lengthOfNewBits > 0);
        varintBitstreamSet(x, *bitsUsed, BITS_DATA, lengthOfNewBits);
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

    *currentLengthOfBits = varintBitstreamGet(x, *bitOffset, BITS_DATA);
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
 * Friendly Interface
 * ==================================================================== */
/* TODO! */

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

    TEST_FINAL_RESULT;
}
#endif
