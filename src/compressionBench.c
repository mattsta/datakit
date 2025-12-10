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

#include "datakit.h"
#include "dod.h"
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
    dod *encoded = zcalloc(1, maxBytes);
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

    int64_t *decoded = zmalloc(count * sizeof(int64_t));

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

    zfree(encoded);
    zfree(decoded);
}

/* varint delta benchmark (ZigZag + varintExternal) */
static void benchVarintDelta(BenchReport *r, const int64_t *values,
                             size_t count) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = zmalloc(maxSize);
    size_t encodedSize = varintDeltaEncode(encoded, values, count);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    int64_t *decoded = zmalloc(count * sizeof(int64_t));
    varintDeltaDecode(encoded, count, decoded);

    uint64_t endDecode = timeUtilMonotonicUs();

    /* Verify */
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }

    benchReportAdd(r, "varintDelta (ZigZag)", encodedSize,
                   (double)(endEncode - startEncode),
                   (double)(endDecode - startDecode));

    zfree(encoded);
    zfree(decoded);
}

/* varintBP128 delta benchmark (SIMD-optimized) */
static void benchVarintBP128(BenchReport *r, const int64_t *values,
                             size_t count) {
    /* Convert to uint32_t for BP128 (typical deltas fit in 32 bits) */
    uint32_t *values32 = zmalloc(count * sizeof(uint32_t));
    for (size_t i = 0; i < count; i++) {
        /* Convert signed to unsigned via offset (add base to make positive) */
        values32[i] = (uint32_t)(values[i] - values[0]);
    }

    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintBP128MaxBytes(count);
    uint8_t *encoded = zmalloc(maxSize);
    varintBP128Meta meta;
    size_t encodedSize =
        varintBP128DeltaEncode32(encoded, values32, count, &meta);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    uint32_t *decoded32 = zmalloc(count * sizeof(uint32_t));
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

    zfree(values32);
    zfree(encoded);
    zfree(decoded32);
}

/* ============================================================================
 * Floating Point Compression Benchmarks
 * ============================================================================
 */

/* datakit xof benchmark */
static void benchXof(BenchReport *r, const double *values, size_t count) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    xofWriter w = {0};
    w.d = zcalloc(1, count * 16); /* Conservative allocation */
    w.totalBytes = count * 16;

    for (size_t i = 0; i < count; i++) {
        xofWrite(&w, values[i]);
    }

    uint64_t endEncode = timeUtilMonotonicUs();
    double encodeTimeUs = (double)(endEncode - startEncode);

    size_t encodedBytes = (w.usedBits + 7) / 8;

    double *decoded = zmalloc(count * sizeof(double));

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

    zfree(w.d);
    zfree(decoded);
}

/* varintFloat benchmark */
static void benchVarintFloat(BenchReport *r, const double *values, size_t count,
                             varintFloatPrecision precision, const char *name) {
    /* Encode */
    uint64_t startEncode = timeUtilMonotonicUs();

    size_t maxSize = varintFloatMaxEncodedSize(count, precision);
    uint8_t *encoded = zmalloc(maxSize);
    size_t encodedSize = varintFloatEncode(encoded, values, count, precision,
                                           VARINT_FLOAT_MODE_INDEPENDENT);

    uint64_t endEncode = timeUtilMonotonicUs();

    /* Decode */
    uint64_t startDecode = timeUtilMonotonicUs();

    double *decoded = zmalloc(count * sizeof(double));
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

    zfree(encoded);
    zfree(decoded);
}

/* ============================================================================
 * Main Benchmark Runner
 * ============================================================================
 */

static void runIntegerBenchmarks(size_t count, const char *sizeLabel) {
    char category[128];
    char dataDesc[128];

    /* Test 1: Monotonic timestamps with jitter */
    int64_t *data = zmalloc(count * sizeof(int64_t));
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

    zfree(data);
}

static void runFloatBenchmarks(size_t count, const char *sizeLabel) {
    char category[128];
    char dataDesc[128];

    double *data = zmalloc(count * sizeof(double));

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

    zfree(data);
}

#ifdef DATAKIT_TEST
#include "ctest.h"

int compressionBenchTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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
    runIntegerBenchmarks(1000000, "LARGE");
    runFloatBenchmarks(1000000, "LARGE");

    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━━━\n");
    printf("                           XLARGE DATASET (10M ~ 80MB)             "
           "             \n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
           "━━━━━━━━━━━━━━\n");
    runIntegerBenchmarks(10000000, "XLARGE");
    runFloatBenchmarks(10000000, "XLARGE");

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
    printf("║  FLOAT:   xof excels on smooth time series (bit-level XOR "
           "tracking)           ║\n");
    printf("║           varintFloat offers precision/space tradeoffs (lossy "
           "modes)          ║\n");
    printf("║                                                                  "
           "             ║\n");
    printf("╚══════════════════════════════════════════════════════════════════"
           "═════════════╝\n");

    TEST_FINAL_RESULT;
}
#endif
