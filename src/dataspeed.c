#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "perf.h"

#define ustime _perfTimeUs

#define DURATION_SEC (durationTotal / 1e6)
#define SIZE(i) (sizeBytes * i)
#define CURRENT_GEEBEES(i) SIZE(i) / DURATION_SEC / (1 << 30)

sig_atomic_t stopProcessing = false;

__attribute__((optnone)) static size_t __attribute__((noinline)) caller(void) {
    return 3;
}

__attribute__((optnone)) static double instruction(void) {
    uint64_t warmup = (1ULL << 25);

    printf("Computing average call overhead...\n");
    size_t start = ustime();
    for (size_t i = 0; i < warmup; i++) {
        caller();
    }
    size_t stop = ustime();

    double syscallLatency = stop - start;
    syscallLatency /= warmup;

    printf("Average call overhead: %.4f ns\n", syscallLatency * 1e3);

    return syscallLatency;
}

__attribute__((optnone)) static double loop(void) {
    const size_t itersTotal = (1 << 25);
    size_t iters = itersTotal;
    printf("Now running a quick CPU speed test for %zd iterations...\n", iters);
    PERF_TIMERS_STAT_START;
    while (iters--)
        ;
    PERF_TIMERS_STAT_STOP(itersTotal);

    uint64_t duration = lps.stat.us.duration;
    double durationS = duration / 1e6;
    printf("Iterations completed in %.3f ms for an execution speed of "
           "%.1f million (subtraction) operations per second using one core.\n",
           duration / (float)1e3, itersTotal / 1e6 / durationS);

    double loopLatency = duration * 1000 / (float)itersTotal;
    printf("Loop latency: %.2f nanoseconds per iteration.\n", loopLatency);

    return loopLatency;
}

__attribute__((optnone)) static double memory(double MB, size_t iterations) {
    size_t sizeBytes = MB * (1 << 20);
    char *mem = malloc(sizeBytes);
    assert(mem);

    printf("Running %zu iterations of writing %.2f MB of memory...\n",
           iterations, MB);

    uint64_t durationTotal = 0;

    PERF_TIMERS_SETUP;
    size_t i = 0;
    for (; i < iterations; i++) {
        if (i && i % 50 == 0) {
            printf("Iterations remaining: %5zu (current throughput: %3.2f "
                   "GiB/s)\n",
                   iterations - i, CURRENT_GEEBEES(i));
        }

        PERF_TIMERS_STAT_START;
        memset(mem, 0xDA, sizeBytes);
        PERF_TIMERS_STAT_STOP(i);

        durationTotal += lps.stat.us.duration;

        if (stopProcessing) {
            break;
        }
    }

    PERF_TIMERS_FINISH_PRINT_RESULTS(i * sizeBytes, "MB");

    uint64_t totalSizeWritten = sizeBytes * i;

    float duration_s = DURATION_SEC;
    printf("Wrote %'" PRIu64
           " B in %.4f seconds for a memory write speed of %'.4f "
           "MiB/s (%'.4f GiB/s)\n",
           totalSizeWritten, duration_s,
           totalSizeWritten / duration_s / (1 << 20),
           totalSizeWritten / duration_s / (1 << 30));

    free(mem);
    return CURRENT_GEEBEES(i);
}

static void doStop(int signal) {
    (void)signal;
    stopProcessing = true;
}

__attribute__((optnone)) size_t dataspeed(double MB, size_t iterations) {
    const sig_t prev = signal(SIGINT, doStop);
    instruction();
    printf("\n");
    loop();
    printf("\n");
    memory(MB, iterations);
    signal(SIGINT, prev);

    return EXIT_SUCCESS;
}
