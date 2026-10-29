#pragma once

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Dataspeed - System Benchmarking
 * ============================================================================
 * Measures this machine: cache and memory bandwidth at working sets sized
 * from the detected caches, memory latency by pointer chasing, the cache
 * sizes the latency curve shows, and CPU operation throughput.  Every
 * measurement runs for a configured minimum time and reports statistics
 * over its samples.  Results are for comparing machines and builds, not
 * for scheduling decisions: they move with load, frequency scaling and
 * thermal state.
 * ============================================================================
 */

/* ----------------------------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedConfig {
    /* Each measurement runs at least this long (default 100 ms) */
    size_t min_duration_ms;
    /* Cap on any calibrated loop's iteration count (default 10^9) */
    size_t max_iterations;
    /* Untimed runs of each bandwidth pass and latency sample before
     * timing (default 3); CPU loops warm up while they calibrate */
    size_t warmup_iterations;
    /* Working set of the memory bandwidth test; 0 sizes it past the
     * largest cache (at least 64 MB) */
    size_t memory_mb;
    bool verbose;         /* print each result as it is measured */
    bool include_latency; /* run the latency tests and the cache sweep */
} dataspeedConfig;

/* The defaults above, verbose and with latency */
dataspeedConfig dataspeedDefaultConfig(void);

/* ----------------------------------------------------------------------------
 * System Information
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedCacheInfo {
    size_t l1d_size;  /* L1 data cache size in bytes (0 if unknown) */
    size_t l1i_size;  /* L1 instruction cache size (0 if unknown) */
    size_t l2_size;   /* L2 cache size in bytes (0 if unknown) */
    size_t l3_size;   /* L3 cache size in bytes (0 if unknown) */
    size_t line_size; /* Cache line size in bytes */
} dataspeedCacheInfo;

typedef struct dataspeedSystemInfo {
    /* CPU information */
    char cpu_model[128]; /* CPU model string */
    size_t cpu_count;    /* Number of logical CPUs */
    double cpu_freq_mhz; /* Estimated CPU frequency in MHz */

    /* Cache information */
    dataspeedCacheInfo cache;

    /* Memory information */
    size_t total_memory; /* Total physical memory in bytes */
    size_t page_size;    /* System page size in bytes */
} dataspeedSystemInfo;

/* What the OS reports (sysctl on macOS, sysfs on Linux) */
void dataspeedGetSystemInfo(dataspeedSystemInfo *info);
void dataspeedPrintSystemInfo(const dataspeedSystemInfo *info);

/* ----------------------------------------------------------------------------
 * Benchmark Results
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedStats {
    double min;
    double max;
    double mean;
    double median;
    double stddev;
    double p95; /* 95th percentile */
    size_t samples;
} dataspeedStats;

typedef struct dataspeedBandwidthResult {
    double bandwidth_gbs; /* median over the samples, GB/s */
    double cycles_per_byte;
    size_t working_set_bytes;
    dataspeedStats stats;
} dataspeedBandwidthResult;

typedef struct dataspeedLatencyResult {
    double latency_ns; /* median over the samples */
    double cycles;     /* counter ticks per load (the TSC or CNTVCT) */
    size_t working_set_bytes;
    dataspeedStats stats;
} dataspeedLatencyResult;

/* ----------------------------------------------------------------------------
 * Benchmarks.  A NULL 'system' detects it; a NULL 'config' takes the
 * defaults.  Working sets: L1 half the L1D, L2 half the L2, L3 half the L3
 * (each at least twice the level below; twice the L2 when no L3 is
 * reported), RAM four times the largest cache and at least 64 MB.
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedCacheResults {
    /* Read bandwidth with the working set in each level */
    dataspeedBandwidthResult l1_bandwidth;
    dataspeedBandwidthResult l2_bandwidth;
    dataspeedBandwidthResult l3_bandwidth;
    dataspeedBandwidthResult mem_bandwidth;

    /* The levels the latency sweep shows: the largest working set before
     * each step up in latency (0: no step found, or include_latency
     * false) */
    size_t detected_l1_size;
    size_t detected_l2_size;
    size_t detected_l3_size;
} dataspeedCacheResults;

void dataspeedBenchmarkCacheHierarchy(dataspeedCacheResults *results,
                                      const dataspeedSystemInfo *system,
                                      const dataspeedConfig *config);

typedef struct dataspeedLatencyResults {
    dataspeedLatencyResult l1_latency;
    dataspeedLatencyResult l2_latency;
    dataspeedLatencyResult l3_latency;
    dataspeedLatencyResult mem_latency;
} dataspeedLatencyResults;

/* Load-to-load latency: a pointer chain in random order through each
 * working set */
void dataspeedBenchmarkLatency(dataspeedLatencyResults *results,
                               const dataspeedSystemInfo *system,
                               const dataspeedConfig *config);

typedef struct dataspeedMemoryResults {
    dataspeedBandwidthResult seq_read;  /* Sequential read */
    dataspeedBandwidthResult seq_write; /* Sequential write (memset) */
    dataspeedBandwidthResult seq_copy;  /* Sequential copy (memcpy) */
} dataspeedMemoryResults;

/* Over 'size_mb' megabytes (0: config->memory_mb, or past the caches) */
void dataspeedBenchmarkMemory(dataspeedMemoryResults *results, size_t size_mb,
                              const dataspeedConfig *config);

typedef struct dataspeedCPUResults {
    /* Dependent-chain throughput, billions of operations a second */
    double int_add_gops;
    double int_mul_gops;
    double int_div_gops;
    double float_add_gops;
    double float_mul_gops;
    double float_div_gops;
    double float_fma_gops; /* Fused multiply-add */

    /* A call to a function that is not inlined */
    double call_overhead_ns;

    /* A conditional branch per element over a fixed pattern (predicted)
     * and over random bits (half mispredicted) */
    double branch_predictable_ns;
    double branch_random_ns;
} dataspeedCPUResults;

void dataspeedBenchmarkCPU(dataspeedCPUResults *results,
                           const dataspeedConfig *config);

/* ----------------------------------------------------------------------------
 * Comprehensive Report
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedReport {
    dataspeedSystemInfo system;
    dataspeedCacheResults cache;
    dataspeedLatencyResults latency;
    dataspeedMemoryResults memory;
    dataspeedCPUResults cpu;
    uint64_t timestamp;      /* Unix seconds at the start */
    double total_duration_s; /* Total benchmark time */
} dataspeedReport;

/* Every benchmark under 'config' (NULL: the defaults) */
void dataspeedRunWithConfig(dataspeedReport *report,
                            const dataspeedConfig *config);

/* The defaults, printing progress only when 'verbose' */
void dataspeedRunAll(dataspeedReport *report, bool verbose);

/* The report as a table, as CSV (metric,value,unit) or as JSON */
void dataspeedPrintReport(const dataspeedReport *report);
void dataspeedPrintReportCSV(const dataspeedReport *report);
void dataspeedPrintReportJSON(const dataspeedReport *report);

/* The command-line run: every benchmark with progress and the report;
 * then, when 'MB' and 'iterations' are positive, 'iterations' memsets of
 * 'MB' megabytes with running throughput (SIGINT stops them) */
size_t dataspeed(double MB, size_t iterations);

#ifdef DATAKIT_TEST
/* The benchmarks themselves: timing-sensitive, so not in ctest */
int dataspeedTest(int argc, char *argv[]);
/* Deterministic: cache layout, number parsing, working sets and the cache
 * sweep's steps on sample input */
int dataspeedParseTest(int argc, char *argv[]);
#endif
