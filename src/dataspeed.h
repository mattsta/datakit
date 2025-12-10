#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Dataspeed - High-Performance System Benchmarking
 * ============================================================================
 * Comprehensive CPU and memory performance measurement with:
 * - Cache hierarchy detection and bandwidth measurement
 * - Memory latency analysis via pointer chasing
 * - SIMD vs scalar throughput comparison
 * - Adaptive iteration calibration for accurate results
 * - Statistical reporting with confidence intervals
 * ============================================================================
 */

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

/* Get system information */
void dataspeedGetSystemInfo(dataspeedSystemInfo *info);

/* Print system information summary */
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
    double bandwidth_gbs; /* Bandwidth in GB/s */
    double cycles_per_byte;
    dataspeedStats stats;
} dataspeedBandwidthResult;

typedef struct dataspeedLatencyResult {
    double latency_ns; /* Latency in nanoseconds */
    double cycles;     /* Latency in CPU cycles */
    dataspeedStats stats;
} dataspeedLatencyResult;

/* ----------------------------------------------------------------------------
 * Cache Hierarchy Benchmark
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedCacheResults {
    /* Bandwidth at different working set sizes */
    dataspeedBandwidthResult l1_bandwidth;  /* In-L1 bandwidth */
    dataspeedBandwidthResult l2_bandwidth;  /* In-L2 bandwidth */
    dataspeedBandwidthResult l3_bandwidth;  /* In-L3 bandwidth */
    dataspeedBandwidthResult mem_bandwidth; /* Main memory bandwidth */

    /* Detected cache sizes from latency cliff analysis */
    size_t detected_l1_size;
    size_t detected_l2_size;
    size_t detected_l3_size;
} dataspeedCacheResults;

/* Run cache hierarchy benchmark */
void dataspeedBenchmarkCacheHierarchy(dataspeedCacheResults *results);

/* ----------------------------------------------------------------------------
 * Memory Latency Benchmark (Pointer Chasing)
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedLatencyResults {
    dataspeedLatencyResult l1_latency;
    dataspeedLatencyResult l2_latency;
    dataspeedLatencyResult l3_latency;
    dataspeedLatencyResult mem_latency;
} dataspeedLatencyResults;

/* Run memory latency benchmark using pointer chasing */
void dataspeedBenchmarkLatency(dataspeedLatencyResults *results);

/* ----------------------------------------------------------------------------
 * Memory Bandwidth Benchmark
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedMemoryResults {
    dataspeedBandwidthResult seq_read;  /* Sequential read (memcpy) */
    dataspeedBandwidthResult seq_write; /* Sequential write (memset) */
    dataspeedBandwidthResult seq_copy;  /* Sequential copy (memcpy) */
} dataspeedMemoryResults;

/* Run memory bandwidth benchmark */
void dataspeedBenchmarkMemory(dataspeedMemoryResults *results, size_t size_mb);

/* ----------------------------------------------------------------------------
 * CPU Operations Benchmark
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedCPUResults {
    /* Operations per second (billions) */
    double int_add_gops;
    double int_mul_gops;
    double int_div_gops;
    double float_add_gops;
    double float_mul_gops;
    double float_div_gops;
    double float_fma_gops; /* Fused multiply-add */

    /* Function call overhead */
    double call_overhead_ns;

    /* Branch prediction */
    double branch_predictable_ns;
    double branch_random_ns;
} dataspeedCPUResults;

/* Run CPU operations benchmark */
void dataspeedBenchmarkCPU(dataspeedCPUResults *results);

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
    uint64_t timestamp;      /* When benchmark was run */
    double total_duration_s; /* Total benchmark time */
} dataspeedReport;

/* Run all benchmarks and generate comprehensive report */
void dataspeedRunAll(dataspeedReport *report, bool verbose);

/* Print comprehensive report */
void dataspeedPrintReport(const dataspeedReport *report);

/* Print report in machine-readable format (CSV or JSON) */
void dataspeedPrintReportCSV(const dataspeedReport *report);
void dataspeedPrintReportJSON(const dataspeedReport *report);

/* ----------------------------------------------------------------------------
 * Original API (preserved for compatibility)
 * ----------------------------------------------------------------------------
 */
size_t dataspeed(double MB, size_t iterations);

/* ----------------------------------------------------------------------------
 * Configuration
 * ----------------------------------------------------------------------------
 */
typedef struct dataspeedConfig {
    size_t min_duration_ms;   /* Minimum test duration (default: 100ms) */
    size_t max_iterations;    /* Maximum iterations per test */
    size_t warmup_iterations; /* Warmup iterations (default: 10) */
    bool verbose;             /* Print progress during benchmarks */
    bool include_latency;     /* Include latency benchmarks (slower) */
} dataspeedConfig;

/* Get default configuration */
dataspeedConfig dataspeedDefaultConfig(void);

/* Run benchmarks with custom configuration */
void dataspeedRunWithConfig(dataspeedReport *report,
                            const dataspeedConfig *config);

#ifdef DATAKIT_TEST
int dataspeedTest(int argc, char *argv[]);
#endif
