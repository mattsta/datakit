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

    uint64_t us = (uint64_t)tv.tv_sec * 1000000UL;
    us += (uint64_t)tv.tv_usec;

    return us;
}

static inline uint64_t _perfTSC(void) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
    /* x86/x64: use RDTSC instruction */
    uint32_t lo = 0;
    uint32_t hi = 0;

    /* ask for something that can't be executed out-of-order
     * to force the next _perf_rdtsc to not get re-ordered. */
    __sync_synchronize();
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__) || defined(__arm64__)
    /* ARM64: use virtual counter register for cycle counting */
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__arm__)
    /* ARM32: use performance monitor cycle counter if available */
    uint32_t val;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(val));
    return val;
#else
    /* Fallback for other architectures: use microsecond timer * 1000 as
     * approximation */
    return _perfTimeUs() * 1000;
#endif
}

typedef struct perfStateGlobal {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;

} perfStateGlobal;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(
    sizeof(perfStateGlobal) == 24,
    "perfStateGlobal size changed! Expected 24 bytes (3×8-byte, ZERO padding). "
    "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(perfStateGlobal) <= 64,
               "perfStateGlobal exceeds single cache line (64 bytes)! "
               "Keep performance measurement structs cache-friendly.");

typedef struct perfStateStat {
    uint64_t start;
    uint64_t stop;
    uint64_t duration;
    double runningMean;
    double runningVariance;
    double stddev;
} perfStateStat;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(
    sizeof(perfStateStat) == 48,
    "perfStateStat size changed! Expected 48 bytes (6×8-byte, ZERO padding). "
    "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(perfStateStat) <= 64,
               "perfStateStat exceeds single cache line (64 bytes)! "
               "Keep performance statistics cache-friendly.");

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

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(perfState) == 144,
               "perfState size changed! Expected 144 bytes (2×perfStateGlobal "
               "+ 2×perfStateStat). "
               "Struct contains nested performance measurement state.");
_Static_assert(sizeof(perfState) <= 192,
               "perfState exceeds 3 cache lines (192 bytes)! "
               "Performance measurement struct spans multiple cache lines by "
               "design (global in line 1, stat across lines 2-3).");

/* local perf state */
static perfState lps = {0};

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
            (double)(dataPoint) - lps.stat.subField.runningMean;               \
        lps.stat.subField.runningMean +=                                       \
            delta / (i + 1); /* assume zero-based indexing */                  \
        lps.stat.subField.runningVariance +=                                   \
            delta * ((dataPoint) - lps.stat.subField.runningMean);             \
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
        lps.stat.us.stddev =                                                   \
            sqrt(lps.stat.us.runningVariance / (double)(totalLoops));          \
    } while (0)

/* GCC -pedantic complains about the %'0.2f format specifier, so let's disable
 * format warnings for that one line of code. */
static void PERF_TIMERS_RESULT_PRINT(const size_t i, const char *units) {
    setlocale(LC_ALL, "");

    PERF_TIMERS_STAT_RESULT(i);

    const double totalSeconds =
        (double)(lps.global.us.stop - lps.global.us.start) / 1e6;
    const double speed = (double)i / totalSeconds;
    const double cycles =
        (double)(lps.global.tsc.stop - lps.global.tsc.start) / (double)i;

    char deviations[128] = {0};

    if (lps.stat.us.runningVariance > 0) {
        if (lps.stat.us.runningMean > 1000) {
            snprintf(deviations, sizeof(deviations), "median %f ms ± %f ms ",
                     lps.stat.us.runningMean / 1e3, lps.stat.us.stddev / 1e3);
        } else {
            snprintf(deviations, sizeof(deviations), "median %f us ± %f us ",
                     lps.stat.us.runningMean, lps.stat.us.stddev);
        }
    }

    _Pragma("GCC diagnostic push");
    _Pragma("GCC diagnostic ignored \"-Wformat\"");
    if (PERF_FIRST == FIRST_SECONDS) {
        printf("%lf seconds at %'0.2f/s (%0.2f cycles per %s)\n", totalSeconds,
               speed, cycles, units);
    } else if (PERF_FIRST == FIRST_RATE) {
        printf("%'0.2f/s for %lf seconds (%0.2f cycles per %s)\n", speed,
               totalSeconds, cycles, units);
    } else {
        if (speed > 10000) {
            printf("%0.4f cycles at %'0.0f/s %s(%lf seconds in %s)\n", cycles,
                   speed, deviations, totalSeconds, units);
        } else {
            printf("%0.4f cycles at %'0.2f/s %s(%lf seconds in %s)\n", cycles,
                   speed, deviations, totalSeconds, units);
        }
    }
    _Pragma("GCC diagnostic pop");

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
