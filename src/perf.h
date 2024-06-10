#pragma once

#include <locale.h>
#include <math.h> /* sqrt */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

/* TODO:
 *   - Refactor for optional ns resolution reporting (see multiTimer.c)
 *   - Refactor ways of requesting result ordering/printing/stats output
 *   - add ability to time an overhead loop then remove overhead from results */

static inline uint64_t _perfTimeUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint64_t us = (uint64_t)tv.tv_sec * 1000000;
    us += tv.tv_usec;

    return us;
}

static inline uint64_t _perfTSC(void) {
#if __has_builtin(__builtin_readcyclecounter) && !__arm64__
    return __builtin_readcyclecounter();
#elif __arm64__
    uint64_t result;
    __asm __volatile("mrs %0, CNTVCT_EL0" : "=&r"(result));
    return result;
#elif __x86_64__
    uint32_t lo = 0;
    uint32_t hi = 0;

    /* ask for something that can't be executed out-of-order
     * to force the next _perf_rdtsc to not get re-ordered. */
    __sync_synchronize();
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
#error "No TSC found?"
#endif
}

typedef struct perfStateGlobal {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;
} perfStateGlobal;

typedef struct perfStateStat {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;
    double runningMean;
    double runningVariance;
    double stddev;
} perfStateStat;

typedef struct perfState {
    struct {
        perfStateGlobal us;
        perfStateGlobal tsc;
    } global;
    struct {
        perfStateStat us;
        perfStateStat tsc;
    } stat;
} perfState;

/* local perf state */
static perfState lps = {{{0}}};

#define PERF_TIMERS_SETUP                                                      \
    do {                                                                       \
        lps = (perfState){.global.us.start = _perfTimeUs(),                    \
                          .global.tsc.start = _perfTSC()};                     \
    } while (0)

/* Simple flag to let you switch output from:
 *   0.775890 seconds at 172,985,510.83/s (15.00 cycles per test)
 * to:
 *   15.38 cycles per test is 168,644,261.64/s (took 0.795863 seconds)
 * to:
 *   3444.3/s for 200 seconds (17.32 cycles per test) */
enum firstThing { FIRST_SECONDS, FIRST_CYCLES, FIRST_RATE };
#define PERF_FIRST FIRST_CYCLES

#ifndef GIMME_CSV
#define GIMME_CSV 0
#endif

#ifndef DOUBLE_NEWLINE
#define DOUBLE_NEWLINE 1
#endif

#define PERF_TIMERS_STAT_START                                                 \
    do {                                                                       \
        lps.stat.us.start = _perfTimeUs();                                     \
    } while (0)

#define _PERF_TIMERS_STAT_STOP(i, subField, dataPoint)                         \
    do {                                                                       \
        const double delta =                                                   \
            (double)(dataPoint)-lps.stat.subField.runningMean;                 \
        lps.stat.subField.runningMean +=                                       \
            delta / (i + 1); /* assume zero-based indexing */                  \
        lps.stat.subField.runningVariance +=                                   \
            delta * ((dataPoint)-lps.stat.subField.runningMean);               \
    } while (0)

#define PERF_TIMERS_STAT_STOP(i)                                               \
    do {                                                                       \
        lps.stat.us.stop = _perfTimeUs();                                      \
        lps.stat.us.duration = lps.stat.us.stop - lps.stat.us.start;           \
        _PERF_TIMERS_STAT_STOP(i, us, lps.stat.us.duration);                   \
    } while (0)

#define PERF_TIMERS_CYCLE_STAT_START                                           \
    do {                                                                       \
        lps.stat.tsc.start = _perfTSC();                                       \
    } while (0)

#define PERF_TIMERS_CYCLE_STAT_STOP(i)                                         \
    do {                                                                       \
        lps.stat.tsc.stop _perfTSC();                                          \
        lps.stat.tsc.duration = lps.stat.tsc.stop - lps.stat.tsc.start;        \
        _PERF_TIMERS_STAT_STOP(i, tsc, lps.stat.tsc.duration);                 \
    } while (0)

#define PERF_TIMERS_STAT_RESULT(totalLoops)                                    \
    do {                                                                       \
        if ((totalLoops) > 0) {                                                \
            lps.stat.us.stddev =                                               \
                sqrt(lps.stat.us.runningVariance / (totalLoops));              \
        }                                                                      \
    } while (0)

/* GCC -pedantic complains about the %'0.2f format specifier, so let's disable
 * format warnings for that one line of code. */
static void PERF_TIMERS_RESULT_PRINT(const size_t i, const char *units) {
    setlocale(LC_ALL, "");

    PERF_TIMERS_STAT_RESULT(i);

    const double totalSeconds =
        (double)(lps.global.us.stop - lps.global.us.start) / 1000000;
    const double speed = totalSeconds > 0 ? i / totalSeconds : 0;
    const uint64_t cyclesTotal = lps.global.tsc.stop - lps.global.tsc.start;
    const double cyclesAverage = i > 0 ? (double)cyclesTotal / i : cyclesTotal;

    char deviations[128] = {0};

    if (lps.stat.us.runningVariance > 0) {
        if (lps.stat.us.runningMean > 1000) {
            snprintf(deviations, sizeof(deviations), "median %f ms ± %f ms ",
                     lps.stat.us.runningMean / 1000, lps.stat.us.stddev / 1000);
        } else {
            snprintf(deviations, sizeof(deviations), "median %f us ± %f us ",
                     lps.stat.us.runningMean, lps.stat.us.stddev);
        }
    }

    _Pragma("GCC diagnostic push")
        _Pragma("GCC diagnostic ignored \"-Wformat\"") if (PERF_FIRST ==
                                                           FIRST_SECONDS) {
        printf("%lf seconds at %'0.2f/s (%0.2f cycles per %s)\n", totalSeconds,
               speed, cyclesAverage, units);
    }
    else if (PERF_FIRST == FIRST_RATE) {
        printf("%'0.2f/s for %lf seconds (%0.2f cycles per %s)\n", speed,
               totalSeconds, cyclesAverage, units);
    }
    else {
        if (speed > 10000) {
            printf("%0.4f cycles at %'0.0f/s %s(%lf seconds in %s)\n",
                   cyclesAverage, speed, deviations, totalSeconds, units);
        } else {
            printf("%0.4f cycles at %'0.2f/s %s(%lf seconds in %s)\n",
                   cyclesAverage, speed, deviations, totalSeconds, units);
        }
    }

    if (GIMME_CSV) {
        printf(":csv %f,%f,%f,%f,%s\n", cyclesAverage, lps.stat.us.runningMean,
               lps.stat.us.stddev, speed, units);
    }

    _Pragma("GCC diagnostic pop")

        if (DOUBLE_NEWLINE) {
        printf("\n");
    }
}

#define PERF_TIMERS_RESULT_PRINT_BYTES(_loops, _bytesPerLoop)                  \
    do {                                                                       \
        const size_t loops = _loops;                                           \
        const size_t bytesPerLoop = _bytesPerLoop;                             \
        const double cyclesPerByte =                                           \
            (double)lps.global.tsc.duration / (loops * bytesPerLoop);          \
        printf("%0.4f cycles per byte\n", cyclesPerByte);                      \
    } while (0)

#define PERF_TIMERS_FINISH                                                     \
    do {                                                                       \
        lps.global.tsc.stop = _perfTSC();                                      \
        lps.global.us.stop = _perfTimeUs();                                    \
        lps.global.tsc.duration = lps.global.tsc.stop - lps.global.tsc.start;  \
        lps.global.us.duration = lps.global.us.stop - lps.global.us.start;     \
    } while (0)

#define PERF_TIMERS_FINISH_PRINT_RESULTS(i, units)                             \
    do {                                                                       \
        PERF_TIMERS_FINISH;                                                    \
        PERF_TIMERS_RESULT_PRINT(i, units);                                    \
    } while (0)

#define PERF_TIMERS_THIS(code, i, units)                                       \
    do {                                                                       \
        PERF_TIMERS_SETUP;                                                     \
        (code);                                                                \
        PERF_TIMERS_FINISH;                                                    \
        PERF_TIMERS_PRINT_RESULTS(i, units);                                   \
    } while (0)
