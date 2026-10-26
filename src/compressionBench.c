/**
 * compressionBench.c - Benchmark comparing datakit compression vs varint
 *
 * Compares compression approaches for numerical time series data:
 *
 * INTEGER COMPRESSION:
 *   - datakit dod: Bit-packed delta-of-delta (Gorilla paper style)
 *   - varintDelta: Byte-aligned ZigZag delta encoding
 *   - varintBP128: SIMD-optimized block-packed delta (128-value blocks)
 *
 * FLOATING POINT COMPRESSION:
 *   - datakit xof: Bit-packed XOR (Gorilla paper style)
 *   - varintFloat: IEEE 754 component separation with precision modes
 *
 * Metrics measured:
 *   1. Memory efficiency: bytes per element
 *   2. Encode throughput: million ops/sec
 *   3. Decode throughput: million ops/sec
 *
 * Dataset sizes: 100 (small), 10000 (medium), 1000000 (large)
 */

#include "compressionBench.h"

#include "cleaner.h"
#include "databox.h"
#include "datakit.h"
#include "dod.h"
#include "flex.h"
#include "float16.h"
#include "mflex.h"
#include "multilist.h"
#include "multilistFull.h"
#include "timeUtil.h"
#include "xof.h"

#include "../deps/varint/src/varintBP128.h"
#include "../deps/varint/src/varintDelta.h"
#include "../deps/varint/src/varintExternal.h"
#include "../deps/varint/src/varintFloat.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(dod *, zfree, DodBuf);
defcleanerName(int64_t *, zfree, I64Buf);
defcleanerName(uint8_t *, zfree, U8Buf);
defcleanerName(uint32_t *, zfree, U32Buf);
defcleanerName(double *, zfree, DoubleBuf);
defcleanerName(multilist *, multilistFree, List);
defcleanerName(multilistFull *, multilistFullFree, ListFull);
defcleanerName(mflex *, mflexFree, Mflex);
defcleanerName(mflexState *, mflexStateFree, MflexState);
defcleanerName(flex *, flexFree, Flex);
defcleanerName(flex *, zfree, FlexBuf);
defcleanerName(float *, zfree, FloatBuf);
defcleanerName(uint16_t *, zfree, U16Buf);

/* ============================================================================
 * Benchmark Result Reporting System
 * ============================================================================
 */

#define BENCH_MAX_RESULTS 16

typedef struct {
    const char *name;
    size_t encodedBytes;
    double bytesPerElem;
    double encodeMops;
    double decodeMops;
    double compressionRatio;
} BenchMetric;

typedef struct {
    BenchMetric results[BENCH_MAX_RESULTS];
    size_t count;
    size_t elementCount;
    size_t rawBytesPerElem;
    const char *category;
    const char *dataDescription;
} BenchReport;

static void benchReportInit(BenchReport *r, const char *category,
                            const char *dataDesc, size_t elemCount,
                            size_t rawBytes) {
    memset(r, 0, sizeof(*r));
    r->category = category;
    r->dataDescription = dataDesc;
    r->elementCount = elemCount;
    r->rawBytesPerElem = rawBytes;
}

static void benchReportAdd(BenchReport *r, const char *name,
                           size_t encodedBytes, double encodeTimeUs,
                           double decodeTimeUs) {
    if (r->count >= BENCH_MAX_RESULTS) {
        return;
    }

    BenchMetric *m = &r->results[r->count++];
    m->name = name;
    m->encodedBytes = encodedBytes;
    m->bytesPerElem = (double)encodedBytes / r->elementCount;
    m->encodeMops = r->elementCount / encodeTimeUs;
    m->decodeMops = r->elementCount / decodeTimeUs;
    m->compressionRatio = (double)r->rawBytesPerElem / m->bytesPerElem;
}

static void benchReportPrint(const BenchReport *r) {
    /* Header */
    double rawKB = (r->elementCount * r->rawBytesPerElem) / 1024.0;

    printf("\n");
    printf(
        "┌────────────────────────────────────────────────────────────────────"
        "─────────────────────┐\n");
    printf("│ %-85s │\n", r->category);
    printf(
        "├────────────────────────────────────────────────────────────────────"
        "─────────────────────┤\n");
    printf("│ Data: %-78s  │\n", r->dataDescription);
    printf("│ Elements: %-10zu   Raw size: %8.2f KB (%zu bytes/elem)           "
           "                  │\n",
           r->elementCount, rawKB, r->rawBytesPerElem);
    printf("├────────────────────────────────┬──────────┬──────────┬──────────┬"
           "──────────┬──────────┤\n");
    printf("│ %-30s │ enc KB   │ bytes/el │ enc M/s  │ dec M/s  │ vs raw   │\n",
           "Algorithm");
    printf("├────────────────────────────────┼──────────┼──────────┼──────────┼"
           "──────────┼──────────┤\n");

    /* Results */
    for (size_t i = 0; i < r->count; i++) {
        const BenchMetric *m = &r->results[i];
        double encKB = m->encodedBytes / 1024.0;
        printf("│ %-30s │ %8.2f │ %8.2f │ %8.2f │ %8.2f │ %7.2fx │\n", m->name,
               encKB, m->bytesPerElem, m->encodeMops, m->decodeMops,
               m->compressionRatio);
    }

    printf("└────────────────────────────────┴──────────┴──────────┴──────────┴"
           "──────────┴──────────┘\n");
}

/* Find best in category */
static void benchReportSummary(const BenchReport *r) {
    if (r->count == 0) {
        return;
    }

    size_t bestCompress = 0, bestEncode = 0, bestDecode = 0;
    for (size_t i = 1; i < r->count; i++) {
        if (r->results[i].compressionRatio >
            r->results[bestCompress].compressionRatio) {
            bestCompress = i;
        }
        if (r->results[i].encodeMops > r->results[bestEncode].encodeMops) {
            bestEncode = i;
        }
        if (r->results[i].decodeMops > r->results[bestDecode].decodeMops) {
            bestDecode = i;
        }
    }

    printf("  Best compression: %s (%.2fx)\n", r->results[bestCompress].name,
           r->results[bestCompress].compressionRatio);
    printf("  Best encode speed: %s (%.2f M/s)\n", r->results[bestEncode].name,
           r->results[bestEncode].encodeMops);
    printf("  Best decode speed: %s (%.2f M/s)\n", r->results[bestDecode].name,
           r->results[bestDecode].decodeMops);
}

/* ============================================================================
 * Test Data Generation
 * ============================================================================
 */

/* Generate monotonic sequence with regular intervals (timestamps, counters) */
static void generateMonotonic(int64_t *out, size_t count, int64_t base,
                              int64_t interval, int jitterPercent) {
    int64_t t = base;
    int jitterRange = (int)(interval * jitterPercent / 100);
    for (size_t i = 0; i < count; i++) {
        out[i] = t;
        int jitter = jitterRange > 0
                         ? (rand() % (jitterRange * 2 + 1)) - jitterRange
                         : 0;
        t += interval + jitter;
    }
}

/* Generate sensor-like integers (memory usage, disk space, etc.) */
static void generateSensorIntegers(int64_t *out, size_t count, int64_t base,
                                   int64_t amplitude) {
    for (size_t i = 0; i < count; i++) {
        /* Sine wave with small noise */
        double variation = amplitude * sin(i * 0.01) + (rand() % 100) - 50;
        out[i] = base + (int64_t)variation;
    }
}

/* Generate smooth sensor-like double values */
static void generateSensorDoubles(double *out, size_t count, double base,
                                  double amplitude) {
    for (size_t i = 0; i < count; i++) {
        /* Sine wave with small noise - simulates temperature, pressure, etc. */
        double variation =
            amplitude * sin(i * 0.01) + ((rand() % 100) - 50) * 0.01;
        out[i] = base + variation;
    }
}

/* Generate random walk doubles (stock prices, etc.) */
static void generateRandomWalkDoubles(double *out, size_t count, double start) {
    out[0] = start;
    for (size_t i = 1; i < count; i++) {
        /* Small random change from previous value */
        double delta = ((rand() % 201) - 100) * 0.01; /* -1.0 to +1.0 */
        out[i] = out[i - 1] + delta;
    }
}

/* ============================================================================
 * Integer Compression Benchmarks
 * ============================================================================
 */

/* datakit dod benchmark (delta-of-delta, bit-packed) */
static void benchDod(BenchReport *r, const int64_t *values, size_t count) {
    /* Need at least 3 values for delta-of-delta to make sense */
    if (count < 3) {
        return;
    }

    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    /* Conservative allocation: worst case is 72 bits per value (8 type + 64
     * data) */
    size_t maxBytes = ((count * 72) / 8) + 16;
    cleanerName(zfree, DodBuf) dod *encoded = zcalloc(1, maxBytes);
    size_t usedBits = 0;

    /* First two values are stored as-is for the delta-of-delta base */
    dodVal t0 = values[0];
    dodVal t1 = values[1];

    /* Encode remaining values using delta-of-delta */
    for (size_t i = 2; i < count; i++) {
        dodAppend(encoded, t0, t1, values[i], &usedBits);
        t0 = t1;
        t1 = values[i];
    }

    uint64_t endEncode = timeUtilMonotonicUs();
    double encodeTimeUs = (double)(endEncode - startEncode);

    /* Add 16 bytes for storing t0 and t1 base values */
    size_t encodedBytes = (usedBits + 7) / 8 + 16;

    cleanerName(zfree, I64Buf) int64_t *decoded =
        zmalloc(count * sizeof(int64_t));

    /* Decode method 1: ReaderNextN (batch via reader API) */
    uint64_t startDecode1 = timeUtilMonotonicUs();
    decoded[0] = values[0];
    decoded[1] = values[1];
    dodReader dr;
    dodReaderInit(&dr, values[0], values[1]);
    dodReaderNextN(&dr, encoded, decoded + 2, count - 2);
    uint64_t endDecode1 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "dod ReaderNextN (batch)", encodedBytes, encodeTimeUs,
                   (double)(endDecode1 - startDecode1));

    /* Decode method 2: ReaderNext one-by-one */
    uint64_t startDecode2 = timeUtilMonotonicUs();
    decoded[0] = values[0];
    decoded[1] = values[1];
    dodReaderInit(&dr, values[0], values[1]);
    for (size_t i = 2; i < count; i++) {
        decoded[i] = dodReaderNext(&dr, encoded);
    }
    uint64_t endDecode2 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "dod ReaderNext (one-by-one)", encodedBytes, encodeTimeUs,
                   (double)(endDecode2 - startDecode2));

    /* Decode method 3: Manual state tracking (legacy pattern) */
    uint64_t startDecode3 = timeUtilMonotonicUs();
    decoded[0] = values[0];
    decoded[1] = values[1];
    t0 = values[0];
    t1 = values[1];
    size_t consumedBits = 0;
    for (size_t i = 2; i < count; i++) {
        dodVal val = dodGet(encoded, &consumedBits, t0, t1, 1);
        decoded[i] = val;
        t0 = t1;
        t1 = val;
    }
    uint64_t endDecode3 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "dod manual state (legacy)", encodedBytes, encodeTimeUs,
                   (double)(endDecode3 - startDecode3));
}

/* varint delta benchmark (ZigZag + varintExternal) */
static void benchVarintDelta(BenchReport *r, const int64_t *values,
                             size_t count) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintDeltaMaxEncodedSize(count);
    cleanerName(zfree, U8Buf) uint8_t *encoded = zmalloc(maxSize);
    size_t encodedSize = varintDeltaEncode(encoded, values, count);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    cleanerName(zfree, I64Buf) int64_t *decoded =
        zmalloc(count * sizeof(int64_t));
    varintDeltaDecode(encoded, count, decoded);

    uint64_t endDecode = timeUtilMonotonicUs();

    /* Verify */
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "varintDelta (ZigZag)", encodedSize,
                   (double)(endEncode - startEncode),
                   (double)(endDecode - startDecode));
}

/* varintBP128 delta benchmark (SIMD-optimized) */
static void benchVarintBP128(BenchReport *r, const int64_t *values,
                             size_t count) {
    /* Convert to uint32_t for BP128 (typical deltas fit in 32 bits) */
    cleanerName(zfree, U32Buf) uint32_t *values32 =
        zmalloc(count * sizeof(uint32_t));
    for (size_t i = 0; i < count; i++) {
        /* Convert signed to unsigned via offset (add base to make positive) */
        values32[i] = (uint32_t)(values[i] - values[0]);
    }

    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintBP128MaxBytes(count);
    cleanerName(zfree, U8Buf) uint8_t *encoded = zmalloc(maxSize);
    varintBP128Meta meta;
    size_t encodedSize =
        varintBP128DeltaEncode32(encoded, values32, count, &meta);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    cleanerName(zfree, U32Buf) uint32_t *decoded32 =
        zmalloc(count * sizeof(uint32_t));
    varintBP128DeltaDecode32(encoded, decoded32, count);

    uint64_t endDecode = timeUtilMonotonicUs();

    /* Verify */
    for (size_t i = 0; i < count; i++) {
        assert(decoded32[i] == values32[i]);
    }

    /* +8 for base value storage */
    benchReportAdd(r, "varintBP128 (SIMD)", encodedSize + 8,
                   (double)(endEncode - startEncode),
                   (double)(endDecode - startDecode));
}

/* ============================================================================
 * Floating Point Compression Benchmarks
 * ============================================================================
 */

/* Scope-exit release of a writer-owned xof stream */
static void benchXofWriterFree_cleanup(xofWriter *w) {
    zfree(w->d);
}

/* datakit xof benchmark */
static void benchXof(BenchReport *r, const double *values, size_t count) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    /* The writer owns its stream and may zrealloc it, so the stream is freed
     * through the writer ('w.d'), never through a copy of the pointer.
     * Preallocated (128 bits per value covers xof's 78-bit worst case) so
     * the timing measures encoding, not growth. */
    cleaner(benchXofWriterFree) xofWriter w = {};
    /* @own: container; free=zfree */
    w.d = zcalloc(1, count * 16);
    w.totalBytes = count * 16;

    for (size_t i = 0; i < count; i++) {
        xofWrite(&w, values[i]);
    }

    uint64_t endEncode = timeUtilMonotonicUs();
    double encodeTimeUs = (double)(endEncode - startEncode);

    size_t encodedBytes = (w.usedBits + 7) / 8;

    cleanerName(zfree, DoubleBuf) double *decoded =
        zmalloc(count * sizeof(double));

    /* Decode method 1: xofReadAll (bulk) */
    uint64_t startDecode1 = timeUtilMonotonicUs();
    xofReadAll(w.d, decoded, count);
    uint64_t endDecode1 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "xof ReadAll (bulk)", encodedBytes, encodeTimeUs,
                   (double)(endDecode1 - startDecode1));

    /* Decode method 2: ReaderNextN (batch via reader API) */
    uint64_t startDecode2 = timeUtilMonotonicUs();
    xofReader xr;
    xofReaderInitFromWriter(&xr, &w);
    decoded[0] = xofReaderCurrent(&xr);
    xofReaderNextN(&xr, w.d, decoded + 1, count - 1);
    uint64_t endDecode2 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "xof ReaderNextN (batch)", encodedBytes, encodeTimeUs,
                   (double)(endDecode2 - startDecode2));

    /* Decode method 3: ReaderNext one-by-one */
    uint64_t startDecode3 = timeUtilMonotonicUs();
    xofReaderInit(&xr, w.d);
    decoded[0] = xofReaderCurrent(&xr);
    for (size_t i = 1; i < count; i++) {
        decoded[i] = xofReaderNext(&xr, w.d);
    }
    uint64_t endDecode3 = timeUtilMonotonicUs();

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "xof ReaderNext (one-by-one)", encodedBytes, encodeTimeUs,
                   (double)(endDecode3 - startDecode3));
}

/* varintFloat benchmark */
static void benchVarintFloat(BenchReport *r, const double *values, size_t count,
                             varintFloatPrecision precision, const char *name) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintFloatMaxEncodedSize(count, precision);
    cleanerName(zfree, U8Buf) uint8_t *encoded = zmalloc(maxSize);
    size_t encodedSize = varintFloatEncode(encoded, values, count, precision,
                                           VARINT_FLOAT_MODE_INDEPENDENT);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    cleanerName(zfree, DoubleBuf) double *decoded =
        zmalloc(count * sizeof(double));
    varintFloatDecode(encoded, count, decoded);

    uint64_t endDecode = timeUtilMonotonicUs();

    /* Verify (precision-dependent) */
    if (precision == VARINT_FLOAT_PRECISION_FULL) {
        for (size_t i = 0; i < count; i++) {
            assert(decoded[i] == values[i]);
        }
    }

    benchReportAdd(r, name, encodedSize, (double)(endEncode - startEncode),
                   (double)(endDecode - startDecode));
}

/* ============================================================================
 * Main Benchmark Runner
 * ============================================================================
 */

static void runIntegerBenchmarks(size_t count, const char *sizeLabel) {
    char category[128];
    char dataDesc[128];

    /* Test 1: Monotonic timestamps with jitter */
    cleanerName(zfree, I64Buf) int64_t *data = zmalloc(count * sizeof(int64_t));
    generateMonotonic(data, count, 1700000000000LL, 1000, 5);

    snprintf(category, sizeof(category),
             "INTEGER COMPRESSION (%s: %zu elements)", sizeLabel, count);
    snprintf(dataDesc, sizeof(dataDesc),
             "Monotonic timestamps (1s interval, 5%% jitter)");

    BenchReport r;
    benchReportInit(&r, category, dataDesc, count, sizeof(int64_t));

    benchDod(&r, data, count);
    benchVarintDelta(&r, data, count);
    benchVarintBP128(&r, data, count);

    benchReportPrint(&r);
    benchReportSummary(&r);

    /* Test 2: Sensor-like integers (memory/disk usage pattern) */
    generateSensorIntegers(data, count, 8000000000LL, 100000);

    snprintf(dataDesc, sizeof(dataDesc),
             "Sensor integers (memory usage pattern)");

    benchReportInit(&r, category, dataDesc, count, sizeof(int64_t));

    benchDod(&r, data, count);
    benchVarintDelta(&r, data, count);
    benchVarintBP128(&r, data, count);

    benchReportPrint(&r);
    benchReportSummary(&r);
}

static void runFloatBenchmarks(size_t count, const char *sizeLabel) {
    char category[128];
    char dataDesc[128];

    cleanerName(zfree, DoubleBuf) double *data =
        zmalloc(count * sizeof(double));

    /* Test 1: Smooth sensor data */
    generateSensorDoubles(data, count, 25.0, 5.0);

    snprintf(category, sizeof(category), "FLOAT COMPRESSION (%s: %zu elements)",
             sizeLabel, count);
    snprintf(dataDesc, sizeof(dataDesc),
             "Smooth sensor data (temperature pattern)");

    BenchReport r;
    benchReportInit(&r, category, dataDesc, count, sizeof(double));

    benchXof(&r, data, count);
    benchVarintFloat(&r, data, count, VARINT_FLOAT_PRECISION_FULL,
                     "varintFloat FULL (lossless)");
    benchVarintFloat(&r, data, count, VARINT_FLOAT_PRECISION_HIGH,
                     "varintFloat HIGH (~7 digits)");

    benchReportPrint(&r);
    benchReportSummary(&r);

    /* Test 2: Random walk data (stock prices) */
    generateRandomWalkDoubles(data, count, 100.0);

    snprintf(dataDesc, sizeof(dataDesc),
             "Random walk data (stock price pattern)");

    benchReportInit(&r, category, dataDesc, count, sizeof(double));

    benchXof(&r, data, count);
    benchVarintFloat(&r, data, count, VARINT_FLOAT_PRECISION_FULL,
                     "varintFloat FULL (lossless)");

    benchReportPrint(&r);
    benchReportSummary(&r);
}

/* ============================================================================
 * multilist / mflex LZ4 compression
 * ============================================================================
 * Named data patterns, each written through the public APIs:
 *   - log100:    100-byte structured log lines ("2026-10-18T12:MM:SS.mmmZ
 *                INFO req=N path=... status=... bytes=...", space-padded)
 *   - loginLine: "[INFO] User N logged in at timestamp T" (N = i % 1000,
 *                T fixed), about 49 bytes
 *   - int64:     the 64-bit integers 0, 1, 2, ...
 * Footprints come from multilistFull (the tier a multilist of this size
 * runs as), comparing its uncompressed flex bytes with the bytes its
 * nodes are stored in. Timings go through multilist itself. */
static size_t mlLogLine100(char out[101], size_t i) {
    static const char *const paths[] = {"/api/v1/items", "/api/v1/users",
                                        "/healthz", "/api/v1/orders"};
    static const char *const levels[] = {"INFO", "INFO", "INFO", "WARN"};
    int n = snprintf(out, 101,
                     "2026-10-18T12:%02zu:%02zu.%03zuZ %s req=%08zu "
                     "path=%s status=%d bytes=%zu",
                     (i / 60000) % 60, (i / 1000) % 60, i % 1000, levels[i % 4],
                     i, paths[(i * 7) % 4], (i % 50) ? 200 : 500,
                     200 + (i * 37) % 1800);
    while (n < 100) {
        out[n++] = ' ';
    }

    out[100] = '\0';
    return 100;
}

static size_t mlLoginLine(char out[101], size_t i) {
    return (size_t)snprintf(out, 101,
                            "[INFO] User %zu logged in at timestamp %d",
                            i % 1000, 1760788800);
}

static void benchListFootprint(const char *pattern, size_t count,
                               uint32_t depth) {
    cleanerName(multilistFullFree, ListFull) multilistFull *ml =
        multilistFullNew(FLEX_CAP_LEVEL_4096, depth);
    cleanerName(mflexStateFree, MflexState) mflexState *state =
        mflexStateCreate();
    size_t payload = 0;
    for (size_t i = 0; i < count; i++) {
        char line[101];
        const size_t len = strcmp(pattern, "log100") == 0
                               ? mlLogLine100(line, i)
                               : mlLoginLine(line, i);
        payload += len;
        const databox box = databoxNewBytesString(line);
        multilistFullPushByTypeTail(ml, state, &box);
    }

    const size_t flexBytes = multilistFullBytes(ml);
    const size_t stored = multilistFullBytesActual(ml);
    printf("  %-9s x%-8zu depth %u: payload %10zu B, flex %10zu B, stored "
           "%10zu B (%5.1f%% of flex, %5.2fx)\n",
           pattern, count, depth, payload, flexBytes, stored,
           100.0 * (double)stored / (double)flexBytes,
           (double)flexBytes / (double)stored);
}

typedef enum {
    MFLEX_REPEATED,
    MFLEX_SEQUENTIAL,
    MFLEX_RANDOM,
    MFLEX_STRINGS,
    MFLEX_MIXED
} mflexBenchPattern;

static void benchMflexFootprint(const char *name, mflexBenchPattern pattern,
                                size_t count) {
    cleanerName(mflexFree, Mflex) mflex *m = mflexNew();
    cleanerName(mflexStateFree, MflexState) mflexState *state =
        mflexStateCreate();
    uint64_t rng = 0x243f6a8885a308d3ULL;
    for (size_t i = 0; i < count; i++) {
        char s[64];
        switch (pattern) {
        case MFLEX_REPEATED:
            mflexPushSigned(&m, state, 42, FLEX_ENDPOINT_TAIL);
            break;
        case MFLEX_SEQUENTIAL:
            mflexPushSigned(&m, state, (int64_t)i, FLEX_ENDPOINT_TAIL);
            break;
        case MFLEX_RANDOM: {
            /* splitmix64 */
            uint64_t z = (rng += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            mflexPushSigned(&m, state, (int64_t)(z ^ (z >> 31)),
                            FLEX_ENDPOINT_TAIL);
            break;
        }
        case MFLEX_STRINGS: {
            const int n = snprintf(s, sizeof(s), "user:%zu:session:%zu",
                                   i % 500, (i * 7919) % 100000);
            mflexPushBytes(&m, state, s, (size_t)n, FLEX_ENDPOINT_TAIL);
            break;
        }
        case MFLEX_MIXED:
            if (i % 2) {
                mflexPushSigned(&m, state, (int64_t)(i * 3),
                                FLEX_ENDPOINT_TAIL);
            } else {
                const int n = snprintf(s, sizeof(s), "event-%zu", i % 64);
                mflexPushBytes(&m, state, s, (size_t)n, FLEX_ENDPOINT_TAIL);
            }
            break;
        }
    }

    const size_t flexBytes = mflexBytesUncompressed(m);
    const size_t stored = mflexBytesActual(m);
    printf("  mflex %-36s n=%5zu: flex %7zu B, stored %7zu B (%6.2fx, %s)\n",
           name, count, flexBytes, stored, (double)flexBytes / (double)stored,
           mflexIsCompressed(m) ? "compressed" : "uncompressed");
}

/* Push 'count' values at the tail, then pop them all from the head;
 * returns microseconds for the whole cycle */
static double benchListPushPop(const char *pattern, size_t count,
                               uint32_t depth) {
    cleanerName(multilistFree, List) multilist *ml =
        multilistNewLimit(FLEX_CAP_LEVEL_4096, depth);
    cleanerName(mflexStateFree, MflexState) mflexState *state =
        mflexStateCreate();
    char line[101];

    const uint64_t start = timeUtilMonotonicUs();
    for (size_t i = 0; i < count; i++) {
        databox box;
        if (strcmp(pattern, "int64") == 0) {
            box = (databox){.type = DATABOX_SIGNED_64, .data.i64 = (int64_t)i};
        } else {
            mlLogLine100(line, i);
            box = databoxNewBytesString(line);
        }

        multilistPushByTypeTail(&ml, state, &box);
    }

    for (size_t i = 0; i < count; i++) {
        databox got = {};
        multilistPopHead(&ml, state, &got);
        databoxFreeData(&got);
    }

    return (double)(timeUtilMonotonicUs() - start);
}

static int benchCmpDouble(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

#if defined(__SANITIZE_ADDRESS__)
#define BENCH_SANITIZED 1
#elifdef __has_feature
#if __has_feature(address_sanitizer)
#define BENCH_SANITIZED 1
#endif
#endif

static void runMultilistBenchmarks(void) {
    printf("\n  multilist LZ4 footprint (FLEX_CAP_LEVEL_4096 nodes; depth N "
           "leaves N nodes at each end uncompressed, 0 = never compress)\n");
    const uint32_t depths[] = {0, 1, 2, 5};
    for (size_t d = 0; d < countof(depths); d++) {
        benchListFootprint("log100", 1'000'000, depths[d]);
    }

    benchListFootprint("loginLine", 100000, 0);
    benchListFootprint("loginLine", 100000, 1);

    printf("\n  mflex LZ4 footprint (one mflex; stored is what it keeps)\n");
    const size_t counts[] = {512, 4096};
    for (size_t c = 0; c < countof(counts); c++) {
        benchMflexFootprint("repeated integers (42)", MFLEX_REPEATED,
                            counts[c]);
        benchMflexFootprint("sequential integers (0, 1, 2, ...)",
                            MFLEX_SEQUENTIAL, counts[c]);
        benchMflexFootprint("random 64-bit integers", MFLEX_RANDOM, counts[c]);
        benchMflexFootprint("strings \"user:N:session:M\"", MFLEX_STRINGS,
                            counts[c]);
        benchMflexFootprint("mixed integers and \"event-N\"", MFLEX_MIXED,
                            counts[c]);
    }

    /* Timings: medians over rounds that interleave the depths, so drift
     * and background load hit every depth alike. Sanitized builds run one
     * short round as a smoke test; their timings mean nothing. */
#ifdef BENCH_SANITIZED
    constexpr int ROUNDS = 1;
    const size_t count = 100000;
#else
    constexpr int ROUNDS = 7;
    const size_t count = 1'000'000;
#endif
    const uint32_t timedDepths[] = {0, 2, 1};
    const char *const patterns[] = {"int64", "log100"};
    printf("\n  multilist push+pop timings (%zu pushes to the tail, then %zu "
           "pops from the head; median of %d interleaved rounds)\n",
           count, count, ROUNDS);
    for (size_t p = 0; p < 2; p++) {
        double runs[3][ROUNDS];
        for (size_t r = 0; r < ROUNDS; r++) {
            for (size_t d = 0; d < 3; d++) {
                runs[d][r] =
                    benchListPushPop(patterns[p], count, timedDepths[d]);
            }
        }

        for (size_t d = 0; d < 3; d++) {
            qsort(runs[d], ROUNDS, sizeof(double), benchCmpDouble);
            printf("  %-6s depth %u: median %8.1f ms (min %8.1f, max "
                   "%8.1f), %6.0f ns per push+pop\n",
                   patterns[p], timedDepths[d], runs[d][ROUNDS / 2] / 1000.0,
                   runs[d][0] / 1000.0, runs[d][ROUNDS - 1] / 1000.0,
                   runs[d][ROUNDS / 2] * 1000.0 / (double)count);
        }
    }
}

/* ============================================================================
 * float16 conversion and LZ4 (cflex) throughput
 * ============================================================================
 * Medians over interleaved rounds, so background load hits every variant
 * alike. Sanitized builds run one short round as a smoke test. */
#ifdef BENCH_SANITIZED
#define BENCH_CODEC_ROUNDS 1
#else
#define BENCH_CODEC_ROUNDS 7
#endif

static double benchMedian(double *runs, size_t n) {
    qsort(runs, n, sizeof(double), benchCmpDouble);
    return runs[n / 2];
}

#if __F16C__
#define BENCH_FLOAT16_HARDWARE "F16C"
#elifdef __aarch64__
#define BENCH_FLOAT16_HARDWARE "NEON"
#else
#define BENCH_FLOAT16_HARDWARE "none (software path)"
#endif

static void runFloat16Benchmarks(void) {
#ifdef BENCH_SANITIZED
    const size_t count = 100000;
#else
    const size_t count = 4000000;
#endif
    cleanerName(zfree, FloatBuf) float *in = zcalloc(count, sizeof(*in));
    cleanerName(zfree, U16Buf) uint16_t *half = zcalloc(count, sizeof(*half));
    for (size_t i = 0; i < count; i++) {
        in[i] = (float)(20.0 + 5.0 * sin((double)i / 1000.0));
    }

    /* 0 hardware encode, 1 software encode, 2 hardware decode, 3 software
     * decode; ns per value */
    double runs[4][BENCH_CODEC_ROUNDS];
    [[maybe_unused]] volatile float sink = 0;
    for (size_t r = 0; r < BENCH_CODEC_ROUNDS; r++) {
        uint64_t t = timeUtilMonotonicNs();
        for (size_t i = 0; i < count; i++) {
            half[i] = float16Encode(in[i]);
        }
        runs[0][r] = (double)(timeUtilMonotonicNs() - t) / (double)count;

        t = timeUtilMonotonicNs();
        for (size_t i = 0; i < count; i++) {
            half[i] = float16Encode_(in[i]);
        }
        runs[1][r] = (double)(timeUtilMonotonicNs() - t) / (double)count;

        float acc = 0;
        t = timeUtilMonotonicNs();
        for (size_t i = 0; i < count; i++) {
            acc += float16Decode(half[i]);
        }
        runs[2][r] = (double)(timeUtilMonotonicNs() - t) / (double)count;

        t = timeUtilMonotonicNs();
        for (size_t i = 0; i < count; i++) {
            acc += float16Decode_(half[i]);
        }
        runs[3][r] = (double)(timeUtilMonotonicNs() - t) / (double)count;
        sink += acc;
    }

    printf("\n  float16 conversion, %zu values, median of %d interleaved "
           "rounds (hardware path: %s)\n",
           count, BENCH_CODEC_ROUNDS, BENCH_FLOAT16_HARDWARE);
    const double hwEnc = benchMedian(runs[0], BENCH_CODEC_ROUNDS);
    const double swEnc = benchMedian(runs[1], BENCH_CODEC_ROUNDS);
    const double hwDec = benchMedian(runs[2], BENCH_CODEC_ROUNDS);
    const double swDec = benchMedian(runs[3], BENCH_CODEC_ROUNDS);
    printf("  encode: hardware %6.3f ns/value, software %6.3f ns/value "
           "(%.1fx)\n",
           hwEnc, swEnc, swEnc / hwEnc);
    printf("  decode: hardware %6.3f ns/value, software %6.3f ns/value "
           "(%.1fx)\n",
           hwDec, swDec, swDec / hwDec);
}

/* LZ4 throughput through the flex <-> cflex conversions mflex uses, on a
 * ~64 KB flex of 'log100' lines */
static void runLz4Benchmarks(void) {
    cleanerName(flexFree, Flex) flex *f = flexNew();
    for (size_t i = 0; i < 640; i++) {
        char line[101];
        mlLogLine100(line, i);
        flexPushBytes(&f, line, 100, FLEX_ENDPOINT_TAIL);
    }

    const size_t rawBytes = flexBytes(f);
    cleanerName(zfree, U8Buf) uint8_t *cbuf = zcalloc(1, rawBytes);
    if (!flexConvertToCFlex(f, cbuf, rawBytes - 1)) {
        printf("  LZ4: log100 flex did not compress\n");
        return;
    }

    const size_t compressedBytes = cflexBytes(cbuf);
    size_t outLen = rawBytes;
    /* cflexConvertToFlex may zrealloc 'out'; the cleaner frees whatever
     * it ends up pointing at */
    cleanerName(zfree, FlexBuf) flex *out = zcalloc(1, outLen);

#ifdef BENCH_SANITIZED
    const size_t iterations = 50;
#else
    const size_t iterations = 2000;
#endif
    double runs[2][BENCH_CODEC_ROUNDS];
    for (size_t r = 0; r < BENCH_CODEC_ROUNDS; r++) {
        uint64_t t = timeUtilMonotonicNs();
        for (size_t i = 0; i < iterations; i++) {
            flexConvertToCFlex(f, cbuf, rawBytes - 1);
        }
        runs[0][r] = (double)(timeUtilMonotonicNs() - t);

        t = timeUtilMonotonicNs();
        for (size_t i = 0; i < iterations; i++) {
            cflexConvertToFlex(cbuf, &out, &outLen);
        }
        runs[1][r] = (double)(timeUtilMonotonicNs() - t);
    }

    const bool roundTrips = memcmp(out, f, rawBytes) == 0;

    /* MB/s of uncompressed flex bytes */
    const double mb = (double)rawBytes * (double)iterations / 1e6;
    const double compressMBs =
        mb / (benchMedian(runs[0], BENCH_CODEC_ROUNDS) / 1e9);
    const double decompressMBs =
        mb / (benchMedian(runs[1], BENCH_CODEC_ROUNDS) / 1e9);
    printf("\n  LZ4 via flex <-> cflex, log100 flex of %zu B -> %zu B, "
           "median of %d interleaved rounds%s\n",
           rawBytes, compressedBytes, BENCH_CODEC_ROUNDS,
           roundTrips ? "" : " (ROUND TRIP FAILED)");
    printf("  compress %8.0f MB/s, decompress %8.0f MB/s (uncompressed "
           "bytes per second)\n",
           compressMBs, decompressMBs);
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int compressionBenchTest(int, char *[]) {
    int err = 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════"
           "═════════════╗\n");
    printf("║           COMPRESSION BENCHMARK: datakit vs varint               "
           "             ║\n");
    printf("╠══════════════════════════════════════════════════════════════════"
           "═════════════╣\n");
    printf("║                                                                  "
           "             ║\n");
    printf("║  INTEGER COMPRESSION:                                            "
           "             ║\n");
    printf("║    • datakit dod   : Bit-packed delta-of-delta (Gorilla paper)   "
           "             ║\n");
    printf("║    • varintDelta   : Byte-aligned ZigZag delta encoding          "
           "             ║\n");
    printf("║    • varintBP128   : SIMD-optimized block-packed delta           "
           "             ║\n");
    printf("║                                                                  "
           "             ║\n");
    printf("║  FLOAT COMPRESSION:                                              "
           "             ║\n");
    printf("║    • datakit xof   : Bit-packed XOR (Gorilla paper)              "
           "             ║\n");
    printf("║    • varintFloat   : IEEE 754 component separation               "
           "             ║\n");
    printf("║                                                                  "
           "             ║\n");
    printf("║  Metrics: enc KB, bytes/element, encode M/s, decode M/s, vs raw "
           "(ratio)      ║\n");
    printf("╚══════════════════════════════════════════════════════════════════"
           "═════════════╝\n");

    /* Run benchmarks at different scales */
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                              SMALL DATASET (100)                  "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runIntegerBenchmarks(100, "SMALL");
    runFloatBenchmarks(100, "SMALL");

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                             MEDIUM DATASET (10000)                "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runIntegerBenchmarks(10000, "MEDIUM");
    runFloatBenchmarks(10000, "MEDIUM");

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                            LARGE DATASET (1M ~ 8MB)               "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runIntegerBenchmarks(1'000'000, "LARGE");
    runFloatBenchmarks(1'000'000, "LARGE");

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                           XLARGE DATASET (10M ~ 80MB)             "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runIntegerBenchmarks(10'000'000, "XLARGE");
    runFloatBenchmarks(10'000'000, "XLARGE");

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                     MULTILIST / MFLEX LZ4 COMPRESSION             "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runMultilistBenchmarks();
    runLz4Benchmarks();
    runFloat16Benchmarks();

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════"
           "═════════════╗\n");
    printf("║                                   SUMMARY                        "
           "             ║\n");
    printf("╠══════════════════════════════════════════════════════════════════"
           "═════════════╣\n");
    printf("║                                                                  "
           "             ║\n");
    printf("║  INTEGER: dod excels when deltas are consistent (timestamps, "
           "counters)        ║\n");
    printf("║           BP128 fastest for large batches; varintDelta good "
           "general-purpose   ║\n");
    printf("║                                                                  "
           "             ║\n");
    printf("║  FLOAT:   xof excels on repeated or low-precision values (XOR "
           "of shared bits) ║\n");
    printf("║           varintFloat offers precision/space tradeoffs (lossy "
           "modes)          ║\n");
    printf("║                                                                  "
           "             ║\n");
    printf("╚══════════════════════════════════════════════════════════════════"
           "═════════════╝\n");

    TEST_FINAL_RESULT;
}
#endif
