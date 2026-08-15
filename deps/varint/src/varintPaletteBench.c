/* ====================================================================
 * varintPaletteBench — throughput benchmark for varintPalette
 * ====================================================================
 * Not a correctness test (see varintPaletteTest / varintPaletteFuzz).
 * Measures encode and decode throughput on the four distribution shapes
 * that exercise different pipeline paths:
 *   skewed   — geometric over 8 symbols (entropy-coding sweet spot)
 *   constant — single value (1 bit/value floor)
 *   mixed    — 16 hot values + rare outlier blocks (verbatim routing)
 *   unique   — all distinct (fully verbatim path)
 *
 * Used to gate Phase D optimizations: keep only measured wins.
 * Usage: varintPaletteBench [values-per-run] [repeats]
 *   defaults: 1 << 20 values, 15 repeats (median reported). */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, hidden by glibc under strict
 * -std=c11 unless requested. (Apple exposes them unconditionally.) */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "varintPalette.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rngState_ = 88172645463325252ULL;
static uint64_t rng_(void) {
    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
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

typedef void (*fillFn_)(uint64_t *values, size_t count);

static void fillSkewed_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t r = rng_() & 0xFF;
        uint64_t sym = 0;
        for (uint64_t bit = 128; bit > 1 && (r & bit); bit >>= 1) {
            sym++;
        }
        values[i] = 1000 + sym;
    }
}

static void fillConstant_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = 42;
    }
}

static void fillMixed_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = (rng_() % 512 == 0) ? (rng_() | (1ULL << 60)) : i % 16;
    }
}

static void fillUnique_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = (rng_() << 20) ^ i;
    }
}

static void benchOne_(const char *name, fillFn_ fill, size_t count,
                      int repeats) {
    uint64_t *values = malloc(count * sizeof(*values));
    uint64_t *dec = malloc(count * sizeof(*dec));
    uint8_t *enc = malloc(varintPaletteMaxSize(count));
    double *encTimes = malloc((size_t)repeats * sizeof(*encTimes));
    double *decTimes = malloc((size_t)repeats * sizeof(*decTimes));
    if (!values || !dec || !enc || !encTimes || !decTimes) {
        fprintf(stderr, "bench: allocation failed\n");
        exit(2);
    }

    fill(values, count);

    size_t written = 0;
    for (int r = 0; r < repeats; r++) {
        double t0 = now_();
        written = varintPaletteEncode(enc, values, count, NULL);
        encTimes[r] = now_() - t0;
    }
    for (int r = 0; r < repeats; r++) {
        double t0 = now_();
        size_t got = varintPaletteDecode(enc, written, dec, count);
        decTimes[r] = now_() - t0;
        if (got != count) {
            fprintf(stderr, "bench: decode failed on %s\n", name);
            exit(2);
        }
    }
    if (memcmp(dec, values, count * sizeof(*dec)) != 0) {
        fprintf(stderr, "bench: round-trip mismatch on %s\n", name);
        exit(2);
    }

    qsort(encTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    qsort(decTimes, (size_t)repeats, sizeof(double), cmpDouble_);
    const double encMed = encTimes[repeats / 2];
    const double decMed = decTimes[repeats / 2];
    const double mb = (double)(count * sizeof(uint64_t)) / (1024.0 * 1024.0);

    printf("%-9s  %8.2f MB in %7zu B (%5.2f%%)  "
           "enc %7.1f MB/s (%6.1f Mval/s)  dec %7.1f MB/s (%6.1f Mval/s)\n",
           name, mb, written, 100.0 * (double)written / (mb * 1024 * 1024),
           mb / encMed, (double)count / encMed / 1e6, mb / decMed,
           (double)count / decMed / 1e6);

    free(values);
    free(dec);
    free(enc);
    free(encTimes);
    free(decTimes);
}

int main(int argc, char *argv[]) {
    const size_t count =
        (argc > 1) ? strtoull(argv[1], NULL, 0) : ((size_t)1 << 20);
    const int repeats = (argc > 2) ? atoi(argv[2]) : 15;

    printf("varintPaletteBench: %zu values, median of %d runs\n", count,
           repeats);
    benchOne_("skewed", fillSkewed_, count, repeats);
    benchOne_("constant", fillConstant_, count, repeats);
    benchOne_("mixed", fillMixed_, count, repeats);
    benchOne_("unique", fillUnique_, count, repeats);
    return 0;
}
