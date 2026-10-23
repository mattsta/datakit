#pragma once

/* perf.h - timers for the in-test smoke benchmarks.
 *
 * Every measurement reads two clocks:
 *   - monotonic nanoseconds (perfNowNs), the primary measure.  The `us`
 *     fields of perfState are this clock in microseconds.
 *   - the CPU's cheapest counter (_perfTSC): x86-64 RDTSC, which counts
 *     reference cycles, or AArch64 CNTVCT_EL0, a fixed-rate timer (24 MHz
 *     on Apple silicon) that counts no cycles at all.  PERF_COUNTER_CYCLES
 *     says which, and the printers show "cycles" only when they are.
 *
 * Usage, one timed region (per-TU state `lps`, single-threaded):
 *     PERF_TIMERS_SETUP;
 *     for (size_t i = 0; i < n; i++) { work(); }
 *     PERF_TIMERS_FINISH_PRINT_RESULTS(n, "op");
 *   prints, e.g.
 *     12.345 ns per op, 81,004,455/s (12.345 ms for 1,000,000)
 *   (x86-64 adds ", 41.20 cycles per op").  `units` may be a phrase
 *   ("encode with cached results").
 *   Per-sample statistics: PERF_TIMERS_STAT_START / PERF_TIMERS_STAT_STOP
 *   around each sample add "mean 1.2 us ± 0.1 us over N samples".
 *
 * Printing never touches the process locale (thousands separators are
 * formatted here).  GIMME_CSV adds a ":csv ns_per_unit,sample_mean_ns,
 * sample_stddev_ns,units_per_s,units" line; DOUBLE_NEWLINE (default 1)
 * adds a blank line after each result. */

#include <inttypes.h>
#include <math.h> /* sqrt */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Monotonic nanoseconds.  Apple's CLOCK_MONOTONIC ticks in whole
 * microseconds; its CLOCK_MONOTONIC_RAW is the nanosecond clock.  Linux's
 * CLOCK_MONOTONIC is nanoseconds and read without a system call. */
#if defined(__APPLE__)
#define PERF_CLOCK_ID CLOCK_MONOTONIC_RAW
#else
#define PERF_CLOCK_ID CLOCK_MONOTONIC
#endif
static inline uint64_t perfNowNs(void) {
    struct timespec ts;
    clock_gettime(PERF_CLOCK_ID, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* Monotonic microseconds (the same clock). */
static inline uint64_t _perfTimeUs(void) {
    return perfNowNs() / 1000u;
}

/* The CPU counter, ordered after the instructions before it. */
#if defined(__x86_64__)
#define PERF_COUNTER_CYCLES 1
static inline uint64_t _perfTSC(void) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    /* LFENCE keeps RDTSC from executing before the instructions ahead of
     * it complete (Intel SDM, RDTSC); MFENCE orders memory, not RDTSC. */
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
#define PERF_COUNTER_CYCLES 0
static inline uint64_t _perfTSC(void) {
    uint64_t ticks;
    /* ISB: the read happens after the instructions before it. */
    __asm__ __volatile__("isb\n\tmrs %0, CNTVCT_EL0" : "=r"(ticks)::"memory");
    return ticks;
}
#elif __has_builtin(__builtin_readcyclecounter)
#define PERF_COUNTER_CYCLES 1
static inline uint64_t _perfTSC(void) {
    return __builtin_readcyclecounter();
}
#else
#error "perf.h: no cycle counter for this target"
#endif

/* Running statistics of samples (Welford). */
typedef struct perfStateStat {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;
    uint64_t samples;
    double runningMean;
    double runningVariance; /* sum of squared deviations */
    double stddev;          /* population, set by PERF_TIMERS_STAT_RESULT */
} perfStateStat;

typedef struct perfStateGlobal {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;
} perfStateGlobal;

typedef struct perfState {
    struct {
        perfStateGlobal ns;
        perfStateGlobal us;  /* ns / 1000 */
        perfStateGlobal tsc; /* _perfTSC units (see PERF_COUNTER_CYCLES) */
    } global;
    struct {
        perfStateStat ns;  /* per-sample nanoseconds */
        perfStateStat tsc; /* per-sample counter units */
    } stat;
} perfState;

/* local perf state */
static perfState lps = {{{0}}};

static inline void perfStatAdd(perfStateStat *s, double sample) {
    s->samples++;
    const double delta = sample - s->runningMean;
    s->runningMean += delta / (double)s->samples;
    s->runningVariance += delta * (sample - s->runningMean);
}

static inline double perfStatStddev(const perfStateStat *s) {
    return s->samples > 0 ? sqrt(s->runningVariance / (double)s->samples) : 0;
}

/* `v` rounded to `decimals` (0..6) with ',' between thousands, into
 * `out` (at least 40 bytes).  Returns out. */
static inline char *perfFormatGrouped(double v, int decimals, char *out,
                                      size_t cap) {
    if (decimals < 0) {
        decimals = 0;
    } else if (decimals > 6) {
        decimals = 6;
    }
    if (cap == 0) {
        return out;
    }
    /* A value too long for `digits` (past 1e60) is cut; still grouped. */
    char digits[64];
    if (snprintf(digits, sizeof(digits), "%.*f", decimals, v) <= 0) {
        out[0] = '\0';
        return out;
    }
    const char *p = digits;
    size_t o = 0;
    if (*p == '-') {
        if (o + 1 < cap) {
            out[o++] = '-';
        }
        p++;
    }
    size_t intLen = 0;
    while (p[intLen] >= '0' && p[intLen] <= '9') {
        intLen++;
    }
    for (size_t k = 0; k < intLen; k++) {
        if (k > 0 && (intLen - k) % 3 == 0 && o + 1 < cap) {
            out[o++] = ',';
        }
        if (o + 1 < cap) {
            out[o++] = p[k];
        }
    }
    for (const char *q = p + intLen; *q && o + 1 < cap; q++) {
        out[o++] = *q;
    }
    out[o] = '\0';
    return out;
}

#ifndef GIMME_CSV
#define GIMME_CSV 0
#endif

#ifndef DOUBLE_NEWLINE
#define DOUBLE_NEWLINE 1
#endif

#define PERF_TIMERS_SETUP                                                      \
    do {                                                                       \
        lps = (perfState){0};                                                  \
        lps.global.tsc.start = _perfTSC();                                     \
        lps.global.ns.start = perfNowNs();                                     \
        lps.global.us.start = lps.global.ns.start / 1000u;                     \
    } while (0)

#define PERF_TIMERS_FINISH                                                     \
    do {                                                                       \
        lps.global.ns.stop = perfNowNs();                                      \
        lps.global.tsc.stop = _perfTSC();                                      \
        lps.global.us.stop = lps.global.ns.stop / 1000u;                       \
        lps.global.ns.duration = lps.global.ns.stop - lps.global.ns.start;     \
        lps.global.us.duration = lps.global.us.stop - lps.global.us.start;     \
        lps.global.tsc.duration = lps.global.tsc.stop - lps.global.tsc.start;  \
    } while (0)

/* One sample.  The argument (the caller's zero-based sample index) is not
 * needed: samples are counted here. */
#define PERF_TIMERS_STAT_START                                                 \
    do {                                                                       \
        lps.stat.ns.start = perfNowNs();                                       \
    } while (0)

#define PERF_TIMERS_STAT_STOP(i)                                               \
    do {                                                                       \
        (void)(i);                                                             \
        lps.stat.ns.stop = perfNowNs();                                        \
        lps.stat.ns.duration = lps.stat.ns.stop - lps.stat.ns.start;           \
        perfStatAdd(&lps.stat.ns, (double)lps.stat.ns.duration);               \
    } while (0)

#define PERF_TIMERS_CYCLE_STAT_START                                           \
    do {                                                                       \
        lps.stat.tsc.start = _perfTSC();                                       \
    } while (0)

#define PERF_TIMERS_CYCLE_STAT_STOP(i)                                         \
    do {                                                                       \
        (void)(i);                                                             \
        lps.stat.tsc.stop = _perfTSC();                                        \
        lps.stat.tsc.duration = lps.stat.tsc.stop - lps.stat.tsc.start;        \
        perfStatAdd(&lps.stat.tsc, (double)lps.stat.tsc.duration);             \
    } while (0)

/* Kept for callers: the stddev is computed from the samples counted. */
#define PERF_TIMERS_STAT_RESULT(totalLoops)                                    \
    do {                                                                       \
        (void)(totalLoops);                                                    \
        lps.stat.ns.stddev = perfStatStddev(&lps.stat.ns);                     \
        lps.stat.tsc.stddev = perfStatStddev(&lps.stat.tsc);                   \
    } while (0)

/* A duration in the unit that reads best, into `out` (>= 32 bytes). */
static inline const char *perfFormatNs(double ns, char *out, size_t cap) {
    if (ns >= 1e9) {
        snprintf(out, cap, "%.3f s", ns / 1e9);
    } else if (ns >= 1e6) {
        snprintf(out, cap, "%.3f ms", ns / 1e6);
    } else if (ns >= 1e3) {
        snprintf(out, cap, "%.3f us", ns / 1e3);
    } else {
        snprintf(out, cap, "%.3f ns", ns);
    }
    return out;
}

/* Print the region timed by SETUP/FINISH: `count` units of work. */
static void __attribute__((unused))
PERF_TIMERS_RESULT_PRINT(const size_t count, const char *units) {
    PERF_TIMERS_STAT_RESULT(count);
    const double ns = (double)lps.global.ns.duration;
    const double seconds = ns / 1e9;
    const double perUnit = count > 0 ? ns / (double)count : ns;
    const double rate = seconds > 0 ? (double)count / seconds : 0;

    char perUnitText[32];
    char rateText[48];
    char countText[48];
    printf("%s per %s, %s/s",
           perfFormatNs(perUnit, perUnitText, sizeof(perUnitText)), units,
           perfFormatGrouped(rate, rate >= 100 ? 0 : 2, rateText,
                             sizeof(rateText)));
    if (PERF_COUNTER_CYCLES && count > 0) {
        printf(", %.2f cycles per %s",
               (double)lps.global.tsc.duration / (double)count, units);
    }
    if (lps.stat.ns.samples > 1) {
        char meanText[32];
        char sdText[32];
        printf(
            ", mean %s ± %s over %" PRIu64 " samples",
            perfFormatNs(lps.stat.ns.runningMean, meanText, sizeof(meanText)),
            perfFormatNs(lps.stat.ns.stddev, sdText, sizeof(sdText)),
            lps.stat.ns.samples);
    }
    char totalText[32];
    printf(" (%s for %s)\n", perfFormatNs(ns, totalText, sizeof(totalText)),
           perfFormatGrouped((double)count, 0, countText, sizeof(countText)));

    if (GIMME_CSV) {
        printf(":csv %f,%f,%f,%f,%s\n", perUnit, lps.stat.ns.runningMean,
               lps.stat.ns.stddev, rate, units);
    }
    if (DOUBLE_NEWLINE) {
        printf("\n");
    }
}

/* Throughput of the region timed by SETUP/FINISH over loops *
 * bytesPerLoop bytes. */
#define PERF_TIMERS_RESULT_PRINT_BYTES(_loops, _bytesPerLoop)                  \
    do {                                                                       \
        const double perfBytes_ = (double)(_loops) * (double)(_bytesPerLoop);  \
        const double perfNs_ = (double)lps.global.ns.duration;                 \
        if (perfBytes_ > 0 && perfNs_ > 0) {                                   \
            printf("%0.4f ns/byte, %0.1f MB/s", perfNs_ / perfBytes_,          \
                   perfBytes_ * 1e3 / perfNs_);                                \
            if (PERF_COUNTER_CYCLES) {                                         \
                printf(", %0.4f cycles/byte",                                  \
                       (double)lps.global.tsc.duration / perfBytes_);          \
            }                                                                  \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define PERF_TIMERS_FINISH_PRINT_RESULTS(i, units)                             \
    do {                                                                       \
        PERF_TIMERS_FINISH;                                                    \
        PERF_TIMERS_RESULT_PRINT(i, units);                                    \
    } while (0)

/* Time `code`, run once, as `i` units. */
#define PERF_TIMERS_THIS(code, i, units)                                       \
    do {                                                                       \
        PERF_TIMERS_SETUP;                                                     \
        code;                                                                  \
        PERF_TIMERS_FINISH_PRINT_RESULTS(i, units);                            \
    } while (0)
