/**
 * dd_column_store.c - varintDDStream + varintDD in a column store
 *
 * Combines the two double-double modules into the pattern they were
 * built for: a columnar store that keeps high-precision measurements
 * compressed on disk, and computes aggregates over them without ever
 * losing precision to the aggregation itself.
 *
 * Two ideas carry the design:
 *
 *   Per-column precision policy. Not every column needs 106 bits. A
 *   column of instrument readings that were doubles to begin with keeps
 *   all of them for free; a derived column of ratios might only need 20
 *   trailing bits. varintDDStream takes that as one integer, so the
 *   policy lives in the schema rather than in the storage code.
 *
 *   Exact aggregation. A column store's whole job is scanning millions
 *   of values and reducing them. That is precisely where naive double
 *   summation quietly falls apart, and precisely what varintDDSumArray
 *   and varintDDDotDoubles fix - at no cost, since they tend to run
 *   faster than the naive loop anyway.
 *
 * Compile: gcc -I../../src dd_column_store.c ../../src/varintDDStream.c
 *          ../../src/varintDD.c ../../src/varintTagged.c -lm -o dd_column_store
 * Run:     ./dd_column_store
 */

#include "varintDDStream.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 50000

static const char *dd(varintDD value) {
    static char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), value, 30);
    return text;
}

static uint64_t rngState = 0x853C49E6748FEA9BULL;

static uint64_t rng(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return rngState;
}

/* ====================================================================
 * A stored column
 * ==================================================================== */

typedef struct ddColumn {
    const char *name;
    const char *note;
    uint8_t trailingBits; /* the precision policy for this column */
    uint8_t *bytes;       /* the compressed payload */
    size_t byteCount;
    size_t rows;
    varintDDStreamMeta meta;
} ddColumn;

static bool columnStore(ddColumn *column, const varintDD *values, size_t rows) {
    const size_t capacity = varintDDStreamMaxSize(rows);

    column->bytes = malloc(capacity);

    if (column->bytes == NULL) {
        return false;
    }

    column->rows = rows;
    column->byteCount = varintDDStreamEncode(
        column->bytes, values, rows, VARINT_DD_STREAM_HI_AUTO,
        column->trailingBits, &column->meta);

    if (column->byteCount == 0) {
        free(column->bytes);
        column->bytes = NULL;
        return false;
    }

    /* Hand back the slack; a stored column is long-lived and the worst
     * case bound is generous by design. */
    uint8_t *shrunk = realloc(column->bytes, column->byteCount);

    if (shrunk != NULL) {
        column->bytes = shrunk;
    }

    return true;
}

/* Scan a column into a caller-provided buffer. Returns rows produced,
 * or 0 if the stored bytes are unusable - which for a store reading
 * back its own files means corruption, and must not be papered over. */
static size_t columnScan(const ddColumn *column, varintDD *out,
                         size_t capacity) {
    return varintDDStreamDecode(column->bytes, column->byteCount, out,
                                capacity);
}

static void columnFree(ddColumn *column) {
    free(column->bytes);
    column->bytes = NULL;
}

/* ====================================================================
 * Aggregates
 * ==================================================================== */

/* Sum, mean, and variance in one pass, all at double-double width.
 *
 * Variance uses the two-pass form deliberately. The textbook one-pass
 * shortcut (E[x^2] - E[x]^2) subtracts two large nearly-equal numbers
 * and is the classic way to compute a NEGATIVE variance; carrying 106
 * bits would only postpone that, not fix it. Precision is not a
 * substitute for a numerically sound formula - it buys headroom for
 * one that is already sound. */
typedef struct ddStats {
    varintDD sum;
    varintDD mean;
    varintDD variance;
} ddStats;

static ddStats columnStats(const varintDD *values, size_t rows) {
    ddStats stats = {varintDDZero(), varintDDZero(), varintDDZero()};

    if (rows == 0) {
        return stats;
    }

    stats.sum = varintDDSumArray(values, rows);
    stats.mean = varintDDDivDouble(stats.sum, (double)rows);

    varintDDAccum spread = varintDDAccumInit();

    for (size_t i = 0; i < rows; i++) {
        const varintDD delta = varintDDSub(values[i], stats.mean);
        const varintDD square = varintDDSquare(delta);

        /* The accumulator takes doubles, so feed it both limbs: the
         * leading one through the compensated add, the trailing one
         * straight into the compensation term where it belongs. */
        varintDDAccumAdd(&spread, square.hi);
        spread.comp += square.lo;
    }

    stats.variance =
        varintDDDivDouble(varintDDAccumResult(&spread), (double)rows);
    return stats;
}

int main(void) {
    printf("========================================\n");
    printf("Double-double column store\n");
    printf("========================================\n");

    if (!varintDDSelfCheck()) {
        printf("varintDDSelfCheck failed; see example_dd.c\n");
        return 1;
    }

    static varintDD pressure[ROWS];
    static varintDD ratio[ROWS];
    static varintDD scratch[ROWS];

    /* Column 1: barometric pressure, exactly representable readings
     * drifting slowly. Nothing here needs 106 bits, but storing it as
     * double-double costs almost nothing and keeps the column type
     * uniform across the schema. */
    {
        double walk = 1013.25;

        for (size_t i = 0; i < ROWS; i++) {
            walk += (double)(int64_t)(rng() % 21) * 0.01 - 0.1;
            pressure[i] = varintDDFromDouble(walk);
        }
    }

    /* Column 2: a derived ratio. Division produces a full trailing
     * mantissa, so this is genuine double-double data. */
    for (size_t i = 0; i < ROWS; i++) {
        ratio[i] = varintDDDiv(pressure[i],
                               varintDDFromDouble(1013.25 + (double)(i % 7)));
    }

    ddColumn columns[3] = {
        {"pressure",
         "instrument readings, lossless",
         VARINT_DD_STREAM_LOSSLESS,
         NULL,
         0,
         0,
         {0}},
        {"ratio_full",
         "derived ratio, lossless",
         VARINT_DD_STREAM_LOSSLESS,
         NULL,
         0,
         0,
         {0}},
        {"ratio_20bit", "derived ratio, 20 trailing bits", 20, NULL, 0, 0, {0}},
    };

    if (!columnStore(&columns[0], pressure, ROWS) ||
        !columnStore(&columns[1], ratio, ROWS) ||
        !columnStore(&columns[2], ratio, ROWS)) {
        printf("failed to store columns\n");
        return 1;
    }

    printf("\n%zu rows per column, %zu bytes each uncompressed\n\n",
           (size_t)ROWS, ROWS * sizeof(varintDD));
    printf("  %-12s %-32s %9s %8s %7s\n", "column", "policy", "bytes",
           "per row", "ratio");

    for (size_t c = 0; c < 3; c++) {
        printf("  %-12s %-32s %9zu %8.2f %6.2fx\n", columns[c].name,
               columns[c].note, columns[c].byteCount,
               (double)columns[c].byteCount / ROWS,
               (double)(ROWS * sizeof(varintDD)) /
                   (double)columns[c].byteCount);
    }

    /* ---- scan and aggregate ---- */
    printf("\n--- Aggregates over the pressure column ---\n\n");

    if (columnScan(&columns[0], scratch, ROWS) != ROWS) {
        printf("column scan failed\n");
        return 1;
    }

    const ddStats stats = columnStats(scratch, ROWS);

    printf("  sum       %s\n", dd(stats.sum));
    printf("  mean      %s\n", dd(stats.mean));
    printf("  variance  %s\n", dd(stats.variance));
    printf("  std dev   %s\n\n", dd(varintDDSqrt(stats.variance)));

    /* The same aggregate the way most code does it, for contrast. */
    {
        double naive = 0.0;

        for (size_t i = 0; i < ROWS; i++) {
            naive += varintDDToDouble(scratch[i]);
        }

        const varintDD gap = varintDDSub(stats.sum, varintDDFromDouble(naive));

        printf("  naive double sum   %.17g\n", naive);
        printf("  compensated sum    %.17g\n", varintDDToDouble(stats.sum));
        printf("  they differ by     %.3e\n", fabs(varintDDToDouble(gap)));
        printf("\n  Only %zu rows, so the gap is small - but it grows with\n",
               (size_t)ROWS);
        printf("  row count, and a column store exists to scan a lot more\n");
        printf("  than %zu rows.\n", (size_t)ROWS);
    }

    /* ---- what the lossy column actually cost ---- */
    printf("\n--- What the 20-bit policy gave up ---\n\n");

    if (columnScan(&columns[2], scratch, ROWS) != ROWS) {
        printf("column scan failed\n");
        return 1;
    }

    double worst = 0.0;

    for (size_t i = 0; i < ROWS; i++) {
        const varintDD diff = varintDDSub(scratch[i], ratio[i]);
        const double relative =
            fabs(varintDDToDouble(diff)) / fabs(ratio[i].hi);

        if (relative > worst) {
            worst = relative;
        }
    }

    printf("  declared bound  %.3e\n", varintDDStreamMaxRelativeError(20));
    printf("  worst observed  %.3e\n", worst);
    printf("  space saved     %.1f%% versus the lossless column\n",
           100.0 * (1.0 - (double)columns[2].byteCount /
                              (double)columns[1].byteCount));

    printf("\n  Still 22 digits of precision - far past what a double\n");
    printf("  could carry - for meaningfully fewer bytes. That trade is\n");
    printf("  a schema decision, which is why it is one integer per\n");
    printf("  column rather than a rebuild of the storage layer.\n");

    for (size_t c = 0; c < 3; c++) {
        columnFree(&columns[c]);
    }

    printf("\n========================================\n");
    return 0;
}
