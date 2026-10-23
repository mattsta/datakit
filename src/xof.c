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
 * Bit views of a double
 * ==================================================================== */
/* memcpy is the defined way to reinterpret the bits; it compiles to a
 * register move. */
static inline uint64_t xofBitsFromDouble(const double val) {
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    return bits;
}

static inline double xofDoubleFromBits(const uint64_t bits) {
    double val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

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

    varintBitstreamSet(x, 0, 64, xofBitsFromDouble(val));
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

    /* __builtin_clzll/ctzll take a 64-bit unsigned argument. */
    const uint64_t compared = o ^ v;
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
        return xofDoubleFromBits(*currentValueBits);
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

/* Longest entry xofAppend() writes: type, leading zeroes, length, 64 bits */
#define XOF_MAX_ENTRY_BITS (BITS_TYPE + BITS_LEADING_ZEROS + BITS_DATA + 64)

/* Make 'w->d' big enough for 'extraBits' more bits after 'usedBits', plus
 * what a reader touches past the end: decoding the last entry reads its
 * 2-bit type, one bit beyond a final 1-bit entry, through whole 64-bit
 * words. Grows with zrealloc (at least doubling) and zeroes the new tail,
 * since repeated values are only skipped bits and rely on it. A NULL
 * 'w->d' is allocated here. */
static void xofWriterReserve(xofWriter *const w, const size_t extraBits) {
    const size_t endBits = w->usedBits + extraBits;
    const size_t needBytes = ((endBits / 64) + 1) * sizeof(xof);
    const size_t haveBytes = w->d ? w->totalBytes : 0;
    if (haveBytes >= needBytes) {
        return;
    }

    size_t newBytes = haveBytes * 2;
    if (newBytes < needBytes) {
        newBytes = needBytes;
    }

    /* @own: container; free=zfree */
    w->d = zrealloc(w->d, newBytes);
    memset((uint8_t *)w->d + haveBytes, 0, newBytes - haveBytes);
    w->totalBytes = newBytes;
}

void xofWrite(xofWriter *const w, const double val) {
    if (w->count == 0) {
        /* First value: use xofInit to write raw 64-bit double. The XOR
         * window starts unset (-1, -1), so the first XOR entry is always
         * a NEW entry: readers start with no window and can only decode
         * a SAME entry after a NEW one has set it. */
        w->usedBits = 0;
        xofWriterReserve(w, 64);
        w->currentLeadingZeroes = -1;
        w->currentTrailingZeroes = -1;
        xofInit(w->d, &w->usedBits, val);
    } else {
        /* Subsequent values: encode as XOR delta from previous */
        xofWriterReserve(w, XOF_MAX_ENTRY_BITS);
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
    vals[0] = xofDoubleFromBits(currentValueBits);

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
    return xofDoubleFromBits(r->currentValueBits);
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
#include "cleaner.h"
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

/* Scope-exit cleaners for test scratch buffers (see cleaner.h idioms). */
defcleanerName(xof *, zfree, Bits);
defcleanerName(double *, zfree, Doubles);

#define RD(max) (((double)rand() / (double)(RAND_MAX)) * (max))

static void xofTestWriterFree_cleanup(xofWriter *w) {
    zfree(w->d);
}

static uint64_t xofTestRand(uint64_t *state) {
    /* splitmix64 */
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Write 'bits' (as doubles) through a writer that starts from NULL
 * (initialBytes 0) or a small zeroed preallocation, then read every value
 * back through every read path: xofReader (Next and NextN), xofGet() and
 * xofReadAll(), comparing exact bits. The writer must keep room for
 * everything a reader touches. */
static int xofCheckGrownWriter(const char *what, const uint64_t *bits,
                               const size_t n, const size_t initialBytes) {
    int err = 0;
    cleaner(xofTestWriterFree) xofWriter w = {0};
    if (initialBytes) {
        /* @own: container; free=zfree */
        w.d = zcalloc(1, initialBytes);
        w.totalBytes = initialBytes;
    }

    for (size_t i = 0; i < n; i++) {
        xofWrite(&w, xofDoubleFromBits(bits[i]));
    }

    if (w.count != n || w.totalBytes < ((w.usedBits / 64) + 1) * sizeof(xof)) {
        ERR("%s: count %zu, %zu bytes for %zu bits", what, w.count,
            w.totalBytes, w.usedBits);
    }

    xofReader r;
    xofReaderInitFromWriter(&r, &w);
    for (size_t i = 0; i < n; i++) {
        const double got =
            i == 0 ? xofReaderCurrent(&r) : xofReaderNext(&r, w.d);
        if (xofBitsFromDouble(got) != bits[i]) {
            ERR("%s: reader [%zu] = %016" PRIx64 ", want %016" PRIx64, what, i,
                xofBitsFromDouble(got), bits[i]);
            break;
        }
    }

    if (r.bitOffset != w.usedBits || xofReaderRemaining(&r, n) != 0) {
        ERR("%s: reader ended at bit %zu, writer at %zu", what, r.bitOffset,
            w.usedBits);
    }

    cleanerName(zfree, Doubles) double *all = zcalloc(n, sizeof(*all));
    xofReader batch;
    xofReaderInitFromWriter(&batch, &w);
    xofReaderNextN(&batch, w.d, all, n - 1);
    for (size_t i = 1; i < n; i++) {
        if (xofBitsFromDouble(all[i - 1]) != bits[i]) {
            ERR("%s: NextN [%zu] wrong", what, i);
            break;
        }
    }

    xofReadAll(w.d, all, n);
    for (size_t i = 0; i < n; i++) {
        if (xofBitsFromDouble(all[i]) != bits[i]) {
            ERR("%s: ReadAll [%zu] wrong", what, i);
            break;
        }
    }

    /* xofGet() replays from the start: check a spread of offsets */
    for (size_t i = 0; i < n; i += 97) {
        if (xofBitsFromDouble(xofGet(w.d, i)) != bits[i]) {
            ERR("%s: xofGet(%zu) wrong", what, i);
            break;
        }
    }

    if (xofBitsFromDouble(xofGet(w.d, n - 1)) != bits[n - 1]) {
        ERR("%s: xofGet(last) wrong", what);
    }

    return err;
}

/* Check all 'n' values of 'bits' against EXPECTED_AT (an expression in
 * 'i'): every value through one sequential reader pass, then xofGet's own
 * random access at the first and last 64 indexes and every 97th.  Calling
 * xofGet for every index would decode from the start each time, O(n^2)
 * for the stream. */
#define XOF_TEST_DECODE_ALL(bits, n, EXPECTED_AT)                              \
    do {                                                                       \
        xofReader r_;                                                          \
        xofReaderInit(&r_, (bits));                                            \
        for (size_t i = 0; i < (n); i++) {                                     \
            const double got =                                                 \
                i == 0 ? xofReaderCurrent(&r_) : xofReaderNext(&r_, (bits));   \
            const double expected = (EXPECTED_AT);                             \
            if (got != expected) {                                             \
                ERR("reader [%'zu] Expected %f but got %f instead!", i,        \
                    expected, got);                                            \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        for (size_t i = 0; i < (n); i++) {                                     \
            if (i >= 64 && (n) - i > 64 && i % 97 != 0) {                      \
                continue;                                                      \
            }                                                                  \
            const double got = xofGet((bits), i);                              \
            const double expected = (EXPECTED_AT);                             \
            if (got != expected) {                                             \
                ERR("xofGet [%'zu] Expected %f but got %f instead!", i,        \
                    expected, got);                                            \
                break;                                                         \
            }                                                                  \
        }                                                                      \
    } while (0)

int xofTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const size_t loopers = 10000;

    for (int32_t val = -1; val < 2; val++) {
        TEST_DESC("all same (%d)", val) {
            cleanerName(zfree, Bits) xof *bits =
                zcalloc(loopers, sizeof(*bits));
            TEST("encode") {
                PERF_TIMERS_SETUP;
                size_t bitsUsed = 0;
                xofInit(bits, &bitsUsed, val);

                /* xofInit wrote value 0; append values 1 .. loopers-1 */
                int_fast32_t prevLZ = -1;
                int_fast32_t prevTZ = -1;
                for (size_t i = 1; i < loopers; i++) {
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

            TEST("decode all") {
                XOF_TEST_DECODE_ALL(bits, loopers, (double)val);
            }
            printf("\n");
        }
    }

    for (int32_t val = -7; val <= 7; val++) {
        TEST_DESC("alternating (%d, %d)", val, val + 1) {
            cleanerName(zfree, Bits) xof *bits =
                zcalloc(loopers, sizeof(*bits));
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

            TEST("decode all") {
                XOF_TEST_DECODE_ALL(bits, loopers,
                                    i % 2 == 0 ? (double)val : (double)val + 1);
            }
            printf("\n");
        }
    }

    TEST("simples") {
        cleanerName(zfree, Bits) xof *bits = zcalloc(loopers, sizeof(*bits));
        TEST("encode") {
            PERF_TIMERS_SETUP;
            size_t bitsUsed = 0;
            xofInit(bits, &bitsUsed, 1);

            /* values 1 .. loopers: index i holds i + 1 */
            double prevVal = 1;
            int_fast32_t prevLZ = -1;
            int_fast32_t prevTZ = -1;
            for (size_t i = 2; i <= loopers; i++) {
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
            double expected = loopers;
            if (got != expected) {
                ERR("Expected %f but got %f instead!\n", expected, got);
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode last");
        }

        TEST("decode all") {
            XOF_TEST_DECODE_ALL(bits, loopers, (double)(i + 1));
        }
        printf("\n");
    }

    TEST("random") {
        cleanerName(zfree, Bits) xof *bits =
            zcalloc(loopers * 2, sizeof(*bits));
        cleanerName(zfree, Doubles) double *values =
            zcalloc(loopers, sizeof(*values));
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

        TEST("decode all") {
            XOF_TEST_DECODE_ALL(bits, loopers, values[i]);
        }
        printf("\n");
    }

    double deltas[] = {0.77,   0.077,   0.0077, 0.00077, 0.33,  0.033,
                       0.0033, 0.00033, 1,      2,       0.0004};
    for (size_t q = 0; q < sizeof(deltas) / sizeof(*deltas); q++) {
        TEST_DESC("random (bounded random delta range; delta %f)", deltas[q]) {
            cleanerName(zfree, Bits) xof *bits =
                zcalloc(loopers * 2, sizeof(*bits));
            cleanerName(zfree, Doubles) double *values =
                zcalloc(loopers, sizeof(*values));
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

            TEST("decode all") {
                XOF_TEST_DECODE_ALL(bits, loopers, values[i]);
            }
            printf("\n");
        }
    }

    for (size_t q = 0; q < 30; q++) {
        TEST_DESC("random (unbounded random iteration %'zu)", q) {
            cleanerName(zfree, Bits) xof *bits =
                zcalloc(loopers * 2, sizeof(*bits));
            cleanerName(zfree, Doubles) double *values =
                zcalloc(loopers, sizeof(*values));
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

            TEST("decode all") {
                XOF_TEST_DECODE_ALL(bits, loopers, values[i]);
            }
            printf("\n");
        }
    }

    /* ================================================================
     * xofReader tests
     * ================================================================ */
    TEST("bit-exact special values") {
        /* -0.0, smallest subnormal, NaNs with payloads, -inf, a repeat (the
         * one-bit SAME path), 1.0 and +0.0: every reader must hand back the
         * exact bits, which a == comparison cannot check. */
        const uint64_t patterns[] = {
            0x8000000000000000ULL, 0x0000000000000001ULL, 0x7FF8000000000001ULL,
            0x7FF0000000000001ULL, 0xFFF0000000000000ULL, 0xFFF0000000000000ULL,
            0x3FF0000000000000ULL, 0x0000000000000000ULL,
        };
        const size_t count = sizeof(patterns) / sizeof(*patterns);

        double values[sizeof(patterns) / sizeof(*patterns)];
        for (size_t i = 0; i < count; i++) {
            values[i] = xofDoubleFromBits(patterns[i]);
        }

        xof bits[64] = {0};
        size_t bitsUsed = 0;
        xofInit(bits, &bitsUsed, values[0]);
        int_fast32_t prevLZ = -1;
        int_fast32_t prevTZ = -1;
        for (size_t i = 1; i < count; i++) {
            xofAppend(bits, &bitsUsed, &prevLZ, &prevTZ, values[i - 1],
                      values[i]);
        }

        /* The stream opens with the first value's raw IEEE-754 bits */
        if (varintBitstreamGet(bits, 0, 64) != patterns[0]) {
            ERR("First 64 bits are 0x%016" PRIx64 ", expected 0x%016" PRIx64,
                (uint64_t)varintBitstreamGet(bits, 0, 64), patterns[0]);
        }

        double all[sizeof(patterns) / sizeof(*patterns)];
        xofReadAll(bits, all, count);

        xofReader r;
        xofReaderInit(&r, bits);
        for (size_t i = 0; i < count; i++) {
            const double viaReader =
                i == 0 ? xofReaderCurrent(&r) : xofReaderNext(&r, bits);
            const uint64_t got[3] = {xofBitsFromDouble(all[i]),
                                     xofBitsFromDouble(xofGet(bits, i)),
                                     xofBitsFromDouble(viaReader)};
            for (size_t k = 0; k < 3; k++) {
                if (got[k] != patterns[i]) {
                    ERR("[%zu] reader %zu gave 0x%016" PRIx64
                        ", expected 0x%016" PRIx64,
                        i, k, got[k], patterns[i]);
                }
            }
        }
    }

    TEST("xofReader - sequential reading") {
        const size_t count = 10000;
        cleanerName(zfree, Bits) xof *bits = zcalloc(count * 2, sizeof(*bits));
        cleanerName(zfree, Doubles) double *values =
            zcalloc(count, sizeof(*values));

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

        printf("xofReader sequential reading: PASS\n");
    }

    TEST("xofReader - batch reading with NextN") {
        const size_t count = 10000;
        cleanerName(zfree, Bits) xof *bits = zcalloc(count * 2, sizeof(*bits));
        cleanerName(zfree, Doubles) double *values =
            zcalloc(count, sizeof(*values));
        cleanerName(zfree, Doubles) double *readBuf =
            zcalloc(count, sizeof(*readBuf));

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

        printf("xofReader batch reading: PASS\n");
    }

    TEST("xofReader - performance comparison O(N) vs O(N^2)") {
        const size_t count = 10000;
        cleanerName(zfree, Bits) xof *bits = zcalloc(count * 2, sizeof(*bits));
        cleanerName(zfree, Doubles) double *values =
            zcalloc(count, sizeof(*values));

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

        printf("xofReader performance comparison: PASS\n");
    }

    TEST("xofReader - xofReaderInitFromWriter") {
        xofWriter w = {0};
        /* @own: container; free=zfree */
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

    TEST("xofWriter - zero-initialized writer starts a fresh XOR window") {
        /* 1.0 then 0xBFF0000000000001 XOR to 0x8000000000000001: no
         * leading or trailing zeroes. With a {0, 0} window left over from
         * zero-initialization the writer used to emit this as a 64-bit
         * SAME entry, which a reader (starting with no window) cannot
         * decode. */
        const double values[] = {1.0,
                                 xofDoubleFromBits(0xBFF0000000000001ULL),
                                 1.0,
                                 3.5,
                                 xofDoubleFromBits(1ULL),
                                 -0.0};
        const size_t count = sizeof(values) / sizeof(*values);

        for (int stale = 0; stale < 2; stale++) {
            xofWriter w = {0};
            if (stale) {
                /* Any leftover window must be ignored too */
                w.currentLeadingZeroes = 0;
                w.currentTrailingZeroes = 63;
            }

            /* @own: container; free=zfree */
            w.d = zcalloc(64, sizeof(*w.d));
            w.totalBytes = 64 * sizeof(*w.d);
            for (size_t i = 0; i < count; i++) {
                xofWrite(&w, values[i]);
            }

            xofReader r;
            xofReaderInitFromWriter(&r, &w);
            double all[sizeof(values) / sizeof(*values)];
            xofReadAll(w.d, all, count);
            for (size_t i = 0; i < count; i++) {
                const double viaReader =
                    i == 0 ? xofReaderCurrent(&r) : xofReaderNext(&r, w.d);
                const uint64_t want = xofBitsFromDouble(values[i]);
                if (xofBitsFromDouble(viaReader) != want ||
                    xofBitsFromDouble(xofGet(w.d, i)) != want ||
                    xofBitsFromDouble(all[i]) != want) {
                    ERR("[%zu] (stale window %d): reader %016" PRIx64
                        ", get %016" PRIx64 ", readAll %016" PRIx64
                        ", want %016" PRIx64,
                        i, stale, xofBitsFromDouble(viaReader),
                        xofBitsFromDouble(xofGet(w.d, i)),
                        xofBitsFromDouble(all[i]), want);
                }
            }

            zfree(w.d);
        }
    }

    TEST("xofWriter - grows well past its initial capacity") {
        const size_t n = 3000;
        cleanerName(zfree, Bits) uint64_t *bits = zcalloc(n, sizeof(*bits));

        /* Random bit patterns: mostly NEW entries, up to 78 bits each */
        uint64_t rng = 0x452821e638d01377ULL;
        for (size_t i = 0; i < n; i++) {
            bits[i] = xofTestRand(&rng);
        }

        err += xofCheckGrownWriter("random from NULL", bits, n, 0);
        err += xofCheckGrownWriter("random from 8 bytes", bits, n, 8);
        err += xofCheckGrownWriter("random from 16 bytes", bits, n, 16);

        /* Repeated values: 1-bit entries, so the stream ends mid-word and
         * the reader's 2-bit type read reaches past the last entry */
        for (size_t i = 0; i < n; i++) {
            bits[i] = xofBitsFromDouble(i < n / 2 ? 42.5 : -7.25);
        }

        err += xofCheckGrownWriter("repeats from NULL", bits, n, 0);
        err += xofCheckGrownWriter("repeats from 8 bytes", bits, n, 8);
    }

    TEST("xofReader - xofReaderRemaining") {
        xofWriter w = {0};
        /* @own: container; free=zfree */
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
