/* ====================================================================
 * varintRecordBench — throughput benchmark for varintRecord
 * ====================================================================
 * Not a correctness test (see varintRecordTest / varintRecordFuzz).
 * Measures encode/decode throughput and compression ratio on record
 * shapes that exercise different strategy lanes and per-column winners:
 *   telemetry — stride timestamps, tiny enum, jittery signed gauge
 *   ticks     — clustered prices, small sizes, sparse bool flags
 *   floats    — smooth F64 + F32 sensor curves (XOR/FLOAT lanes)
 *   ddcol     — double-double column (DD_STREAM lane)
 *   tags      — structured opaque bytes (PLANES lane)
 *   constant  — identical records (stride-growth floor)
 *   noise     — incompressible payloads (VERBATIM floor)
 * Add a row to shapes_[] to grow coverage.
 *
 * Usage: varintRecordBench [records-per-run] [repeats]
 *   defaults: 1 << 20 records, 15 repeats (median reported). */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, hidden by glibc under strict
 * -std=c11 unless requested. (Apple exposes them unconditionally.) */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "varintDD.h"
#include "varintRecord.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rngState_ = UINT64_C(88172645463325252);
static uint64_t rng_(void) {
    rngState_ = rngState_ * UINT64_C(6364136223846793005) +
                UINT64_C(1442695040888963407);
    return rngState_ >> 16;
}

static double now_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmpDouble_(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* --------------------------------------------------------------------
 * Record shapes
 * -------------------------------------------------------------------- */

typedef struct intRow {
    uint64_t timestamp;
    int32_t delta;
    uint16_t size;
    uint8_t flag;
    uint8_t pad_[1];
} intRow;

static const varintRecordField intSchema_[] = {
    VARINT_RECORD_FIELD(intRow, timestamp, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(intRow, delta, VARINT_RECORD_I32),
    VARINT_RECORD_FIELD(intRow, size, VARINT_RECORD_U16),
    VARINT_RECORD_FIELD(intRow, flag, VARINT_RECORD_BOOL),
};

typedef struct floatRow {
    uint64_t timestamp;
    double smooth;
    float sample;
    uint8_t pad_[4];
} floatRow;

static const varintRecordField floatSchema_[] = {
    VARINT_RECORD_FIELD(floatRow, timestamp, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(floatRow, smooth, VARINT_RECORD_F64),
    VARINT_RECORD_FIELD(floatRow, sample, VARINT_RECORD_F32),
};

typedef struct ddRow {
    varintDD value;
} ddRow;

static const varintRecordField ddSchema_[] = {
    VARINT_RECORD_FIELD(ddRow, value, VARINT_RECORD_DD),
};

typedef struct tagRow {
    uint32_t id;
    uint8_t tag[12];
} tagRow;

static const varintRecordField tagSchema_[] = {
    VARINT_RECORD_FIELD(tagRow, id, VARINT_RECORD_U32),
    {offsetof(tagRow, tag), 12, VARINT_RECORD_BYTES, 0},
};

typedef void (*fillFn_)(uint8_t *base, size_t count, size_t stride);

static void fillTelemetry_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        intRow *r = (intRow *)(base + i * stride);
        r->timestamp = UINT64_C(1700000000000) + i * 250;
        r->delta = (int32_t)((int64_t)(rng_() % 41) - 20);
        r->size = (uint16_t)(100 + rng_() % 8);
        r->flag = (rng_() % 16 == 0);
    }
}

static void fillTicks_(uint8_t *base, size_t count, size_t stride) {
    uint64_t price = 5000000;
    for (size_t i = 0; i < count; i++) {
        intRow *r = (intRow *)(base + i * stride);
        price += (rng_() % 7) - 3;
        r->timestamp = price;
        r->delta = (int32_t)((int64_t)(rng_() % 2001) - 1000);
        r->size = (uint16_t)(1 + rng_() % 100);
        r->flag = (rng_() % 64 == 0);
    }
}

static void fillConstant_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        intRow *r = (intRow *)(base + i * stride);
        r->timestamp = 42;
        r->delta = -7;
        r->size = 512;
        r->flag = 1;
    }
}

static void fillNoise_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        intRow *r = (intRow *)(base + i * stride);
        r->timestamp = rng_() | (rng_() << 48);
        r->delta = (int32_t)rng_();
        r->size = (uint16_t)rng_();
        r->flag = (uint8_t)(rng_() & 1);
    }
}

static void fillFloats_(uint8_t *base, size_t count, size_t stride) {
    double smooth = 21.5;
    float sample = 3.25f;
    for (size_t i = 0; i < count; i++) {
        floatRow *r = (floatRow *)(base + i * stride);
        r->timestamp = UINT64_C(1700000000) + i;
        smooth += ((double)(rng_() % 100) - 50.0) * 0.0001;
        r->smooth = smooth;
        sample += (float)((int64_t)(rng_() % 9) - 4) * 0.125f;
        r->sample = sample;
    }
}

static void fillDD_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        ddRow *r = (ddRow *)(base + i * stride);
        r->value.hi = 1000.0 + (double)(rng_() % 100000) * 0.01;
        r->value.lo = 0.0;
    }
}

static void fillTags_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        tagRow *r = (tagRow *)(base + i * stride);
        r->id = 100000 + (uint32_t)i;
        memset(r->tag, 0xA5, 8);
        r->tag[8] = (uint8_t)(rng_() % 4);
        r->tag[9] = (uint8_t)(0xC0 + rng_() % 3);
        r->tag[10] = 0;
        r->tag[11] = (uint8_t)(rng_() % 2);
    }
}

typedef struct shapeRow {
    const char *name;
    fillFn_ fill;
    const varintRecordField *schema;
    size_t fieldCount;
    size_t recordSize;
} shapeRow;

static const shapeRow shapes_[] = {
    {"telemetry", fillTelemetry_, intSchema_, 4, sizeof(intRow)},
    {"ticks", fillTicks_, intSchema_, 4, sizeof(intRow)},
    {"floats", fillFloats_, floatSchema_, 3, sizeof(floatRow)},
    {"ddcol", fillDD_, ddSchema_, 1, sizeof(ddRow)},
    {"tags", fillTags_, tagSchema_, 2, sizeof(tagRow)},
    {"constant", fillConstant_, intSchema_, 4, sizeof(intRow)},
    {"noise", fillNoise_, intSchema_, 4, sizeof(intRow)},
};

static void benchOne_(const shapeRow *shape, size_t count, int repeats) {
    uint8_t *rows = calloc(count, shape->recordSize);
    uint8_t *dec = malloc(count * shape->recordSize);
    uint8_t *enc = malloc(varintRecordMaxEncodedSize(
        count, shape->recordSize, shape->schema, shape->fieldCount));
    double *encTimes = malloc((size_t)repeats * sizeof(*encTimes));
    double *decTimes = malloc((size_t)repeats * sizeof(*decTimes));
    /* The main table reports steady-state throughput (warm reused
     * workspace); the sharded section below quantifies the per-call
     * allocation delta separately. */
    varintRecordCtx *ws = varintRecordCtxNew();
    if (!rows || !dec || !enc || !encTimes || !decTimes || !ws) {
        fprintf(stderr, "bench: allocation failed\n");
        exit(2);
    }
    shape->fill(rows, count, shape->recordSize);

    size_t written = 0;
    varintRecordMeta meta;
    for (int r = 0; r < repeats; r++) {
        const double t0 = now_();
        written = varintRecordEncodeWithCtx(ws, enc, rows, count,
                                            shape->recordSize, shape->schema,
                                            shape->fieldCount, 0, &meta);
        encTimes[r] = now_() - t0;
    }
    size_t decodedCount = 0;
    for (int r = 0; r < repeats; r++) {
        const double t0 = now_();
        const size_t got = varintRecordDecodeWithCtx(
            ws, enc, written, dec, count, shape->recordSize, &decodedCount);
        decTimes[r] = now_() - t0;
        if (got != written || decodedCount != count) {
            fprintf(stderr, "bench: decode failed on %s\n", shape->name);
            exit(2);
        }
    }
    if (memcmp(dec, rows, count * shape->recordSize) != 0) {
        fprintf(stderr, "bench: round-trip mismatch on %s\n", shape->name);
        exit(2);
    }

    qsort(encTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    qsort(decTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    const double encMed = encTimes[repeats / 2];
    const double decMed = decTimes[repeats / 2];
    const double mb = (double)(count * shape->recordSize) / (1024.0 * 1024.0);

    printf("%-9s  %8.2f MB -> %9zu B (%6.2f%%)  "
           "enc %7.1f MB/s  dec %7.1f MB/s  [",
           shape->name, mb, written,
           100.0 * (double)written / (mb * 1024 * 1024), mb / encMed,
           mb / decMed);
    for (size_t f = 0; f < shape->fieldCount; f++) {
        printf("%s%s", f ? " " : "",
               varintRecordStrategyName(
                   (varintRecordStrategy)meta.columnStrategy[f]));
    }
    printf("]\n");

    varintRecordCtxFree(ws);
    free(rows);
    free(dec);
    free(enc);
    free(encTimes);
    free(decTimes);
}

/* Sharding throughput: many small same-schema encodes are where the
 * reusable context pays — per-call allocation cost is amortized to
 * zero after the first shard. */
static void benchSharded_(size_t count, int repeats) {
    enum { SHARD = 4096 };
    const shapeRow *shape = &shapes_[0]; /* telemetry */
    uint8_t *rows = calloc(count, shape->recordSize);
    uint8_t *enc = malloc(varintRecordMaxEncodedSize(
        SHARD, shape->recordSize, shape->schema, shape->fieldCount));
    double *plainTimes = malloc((size_t)repeats * sizeof(*plainTimes));
    double *ctxTimes = malloc((size_t)repeats * sizeof(*ctxTimes));
    varintRecordCtx *ws = varintRecordCtxNew();
    if (!rows || !enc || !plainTimes || !ctxTimes || !ws) {
        fprintf(stderr, "bench: allocation failed\n");
        exit(2);
    }
    shape->fill(rows, count, shape->recordSize);
    const size_t shards = count / SHARD;

    for (int r = 0; r < repeats; r++) {
        double t0 = now_();
        for (size_t s = 0; s < shards; s++) {
            if (varintRecordEncode(enc, rows + s * SHARD * shape->recordSize,
                                   SHARD, shape->recordSize, shape->schema,
                                   shape->fieldCount, 0, NULL) == 0) {
                fprintf(stderr, "bench: shard encode failed\n");
                exit(2);
            }
        }
        plainTimes[r] = now_() - t0;

        t0 = now_();
        for (size_t s = 0; s < shards; s++) {
            if (varintRecordEncodeWithCtx(
                    ws, enc, rows + s * SHARD * shape->recordSize, SHARD,
                    shape->recordSize, shape->schema, shape->fieldCount, 0,
                    NULL) == 0) {
                fprintf(stderr, "bench: ctx shard encode failed\n");
                exit(2);
            }
        }
        ctxTimes[r] = now_() - t0;
    }

    qsort(plainTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    qsort(ctxTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    const double mb =
        (double)(shards * SHARD * shape->recordSize) / (1024.0 * 1024.0);
    const double plainMed = plainTimes[repeats / 2];
    const double ctxMed = ctxTimes[repeats / 2];
    printf("\nsharded encode (%zu shards x %d records, telemetry):\n"
           "  per-call allocation: %7.1f MB/s\n"
           "  reused context:      %7.1f MB/s  (%.2fx)\n",
           shards, SHARD, mb / plainMed, mb / ctxMed, plainMed / ctxMed);

    varintRecordCtxFree(ws);
    free(rows);
    free(enc);
    free(plainTimes);
    free(ctxTimes);
}

int main(int argc, char *argv[]) {
    const size_t count =
        (argc > 1) ? strtoull(argv[1], NULL, 0) : ((size_t)1 << 20);
    const int repeats = (argc > 2) ? atoi(argv[2]) : 15;

    printf("varintRecordBench: %zu records, median of %d runs\n", count,
           repeats);
    for (size_t i = 0; i < sizeof(shapes_) / sizeof(shapes_[0]); i++) {
        benchOne_(&shapes_[i], count, repeats);
    }
    benchSharded_(count, repeats);
    return 0;
}
