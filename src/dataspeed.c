#include "dataspeed.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cleaner.h"
#include "dj.h"
#include "perf.h"
#include "strDoubleFormat.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(size_t *, zfree, Sizes);
defcleanerName(char *, zfree, Chars);

/* SIMD support */
#if defined(__AVX512F__)
#define DATASPEED_USE_AVX512 1
#include <immintrin.h>
#elifdef __AVX2__
#define DATASPEED_USE_AVX2 1
#include <immintrin.h>
#elifdef __SSE2__
#define DATASPEED_USE_SSE2 1
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-macros"
#define DATASPEED_USE_NEON 1
#pragma GCC diagnostic pop
#include <arm_neon.h>
#endif

/* Platform-specific headers */
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#elifdef __linux__
#include <sched.h>
#endif

/* ============================================================================
 * Compiler Optimization Barriers
 * ============================================================================
 */

/* Prevent compiler from reordering or eliminating memory operations */
#define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

/* Prevent compiler from optimizing away a value */
#define DO_NOT_OPTIMIZE(val) __asm__ volatile("" : "+r,m"(val) : : "memory")

/* Clang/GCC specific: disable optimization for a function */
#if defined(__clang__)
#define DATASPEED_NO_OPTIMIZE __attribute__((optnone))
#elifdef __GNUC__
#define DATASPEED_NO_OPTIMIZE __attribute__((optimize("O0")))
#else
#define DATASPEED_NO_OPTIMIZE
#endif

/* Force function to not be inlined */
#define DATASPEED_NOINLINE __attribute__((noinline))

/* Combined: no optimization, no inlining */
#define DATASPEED_BENCHMARK_FUNC DATASPEED_NOINLINE DATASPEED_NO_OPTIMIZE

/* Memory fence for accurate timing - prevents instruction reordering */
#if defined(__aarch64__)
#define MEMORY_FENCE() __asm__ volatile("dmb sy" ::: "memory")
#elifdef __x86_64__
#define MEMORY_FENCE() __asm__ volatile("mfence" ::: "memory")
#else
#define MEMORY_FENCE() COMPILER_BARRIER()
#endif

/* Timing barrier - use before and after timing calls */
#define TIMING_BARRIER()                                                       \
    do {                                                                       \
        MEMORY_FENCE();                                                        \
        COMPILER_BARRIER();                                                    \
    } while (0)

/* Force a pointer to escape, preventing optimization */
#define ESCAPE(p) __asm__ volatile("" : : "g"(p) : "memory")

/* ============================================================================
 * Internal Utilities
 * ============================================================================
 */

static inline uint64_t dataspeed_time_ns(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t timebase = {};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    return mach_absolute_time() * timebase.numer / timebase.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
#endif
}

static inline uint64_t dataspeed_rdtsc(void) {
#if defined(__aarch64__)
    uint64_t result;
    __asm__ __volatile__("mrs %0, CNTVCT_EL0" : "=r"(result));
    return result;
#elifdef __x86_64__
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return dataspeed_time_ns();
#endif
}

/* Compare function for qsort */
static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Compute statistics from samples */
static void compute_stats(double *samples, size_t n, dataspeedStats *stats) {
    if (n == 0) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    /* Sort for median and percentiles */
    qsort(samples, n, sizeof(double), compare_double);

    stats->min = samples[0];
    stats->max = samples[n - 1];
    stats->median = samples[n / 2];
    stats->p95 = samples[(size_t)(n * 0.95)];
    stats->samples = n;

    /* Compute mean */
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += samples[i];
    }
    stats->mean = sum / n;

    /* Compute stddev */
    double variance = 0;
    for (size_t i = 0; i < n; i++) {
        double diff = samples[i] - stats->mean;
        variance += diff * diff;
    }
    stats->stddev = sqrt(variance / n);
}

/* Format bytes to human readable */
static const char *format_bytes(size_t bytes, char *buf, size_t buflen) {
    if (bytes >= (1ULL << 30)) {
        snprintf(buf, buflen, "%.1f GB", (double)bytes / (1ULL << 30));
    } else if (bytes >= (1ULL << 20)) {
        snprintf(buf, buflen, "%.1f MB", (double)bytes / (1ULL << 20));
    } else if (bytes >= (1ULL << 10)) {
        snprintf(buf, buflen, "%.1f KB", (double)bytes / (1ULL << 10));
    } else {
        snprintf(buf, buflen, "%zu B", bytes);
    }
    return buf;
}

/* Parse a sysfs cache size such as "32K\n" into bytes: decimal digits, an
 * optional K or M multiplier, then only blanks or a newline.  Returns false
 * (leaving *bytes alone) on a sign, no digits, any other character, or a
 * size past SIZE_MAX. */
DK_FN_UNUSED static bool dataspeedParseCacheSize_(const char *s,
                                                  size_t *bytes) {
    if (*s < '0' || *s > '9') {
        return false; /* strtoull would accept blanks and a sign */
    }

    errno = 0;
    char *end = NULL;
    const uint64_t count = strtoull(s, &end, 10);
    if (errno != 0 || end == s) {
        return false;
    }

    size_t scale = 1;
    if (*end == 'K') {
        scale = 1024;
        end++;
    } else if (*end == 'M') {
        scale = 1024 * 1024;
        end++;
    }

    while (*end == ' ' || *end == '\t' || *end == '\n') {
        end++;
    }

    size_t size = 0;
    /* The multiply also fails when count itself does not fit size_t */
    if (*end != '\0' || ckd_mul(&size, count, scale)) {
        return false;
    }

    *bytes = size;
    return true;
}

/* Parse a plain sysfs decimal such as "64\n": digits, then only blanks or a
 * newline, at most 'max'.  Returns false (leaving *value alone) on
 * anything else. */
DK_FN_UNUSED static bool dataspeedParseSysfsUInt_(const char *s, uint64_t max,
                                                  uint64_t *value) {
    if (*s < '0' || *s > '9') {
        return false; /* strtoull would accept blanks and a sign */
    }

    errno = 0;
    char *end = NULL;
    const uint64_t parsed = strtoull(s, &end, 10);
    if (errno != 0 || end == s) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\n') {
        end++;
    }

    if (*end != '\0' || parsed > max) {
        return false;
    }

    *value = parsed;
    return true;
}

/* Read the first line of a small sysfs file into buf (NUL-terminated) */
DK_FN_UNUSED static bool dataspeedReadLine_(const char *path, char *buf,
                                            size_t len) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    const bool ok = fgets(buf, (int)len, fp) != NULL;
    fclose(fp);
    return ok;
}

/* ============================================================================
 * Cache Layout Decoding
 * ============================================================================
 * The kernel's cache description is read through a callback so the decoding
 * can be tested against sample layouts.
 */

/* Linux sysfs: write attribute 'attr' ("level", "type", "size",
 * "coherency_line_size") of cache entry 'index' (.../cache/index<N>/) into
 * buf, NUL-terminated; false when that file does not exist */
typedef bool (*dataspeedSysfsReadFn)(void *ctx, int32_t index, const char *attr,
                                     char *buf, size_t len);

/* Apple sysctl: write the integer sysctl 'name' to *value; false when it
 * does not exist */
typedef bool (*dataspeedSysctlReadFn)(void *ctx, const char *name,
                                      int64_t *value);

#define DATASPEED_SYSFS_MAX_INDEX 32
#define DATASPEED_MAX_LINE_SIZE (1 << 20)
#define DATASPEED_DEFAULT_LINE_SIZE 64

/* A sysfs "type" value, without its trailing newline */
DK_FN_UNUSED static bool dataspeedSysfsTypeIs_(const char *type,
                                               const char *want) {
    const size_t n = strlen(want);
    return strncmp(type, want, n) == 0 && (type[n] == '\0' || type[n] == '\n');
}

/* Decode cpu0's sysfs cache entries into 'cache'.  Each entry is placed by
 * its level and type: a level 1 "Data" cache is l1d_size and a level 1
 * "Instruction" cache is l1i_size, whatever order the indices list them in;
 * a level 1 "Unified" cache (or one with no readable type) fills whichever
 * of the two no dedicated entry claimed.  Levels 2 and 3 take any type but
 * "Instruction".  The line size is the L1 data cache's, else the first
 * readable one, else DATASPEED_DEFAULT_LINE_SIZE.  An entry whose level does
 * not parse is skipped; the scan ends at the first index with no level. */
DK_FN_UNUSED static void dataspeedCacheFromSysfs_(dataspeedCacheInfo *cache,
                                                  dataspeedSysfsReadFn read,
                                                  void *ctx) {
    bool l1dDedicated = false;
    bool l1iDedicated = false;
    size_t l1dLine = 0;
    size_t firstLine = 0;

    for (int32_t index = 0; index < DATASPEED_SYSFS_MAX_INDEX; index++) {
        char buf[64];
        uint64_t level = 0;
        if (!read(ctx, index, "level", buf, sizeof(buf))) {
            break; /* no more entries */
        }

        if (!dataspeedParseSysfsUInt_(buf, 16, &level)) {
            continue;
        }

        char type[32] = {};
        if (!read(ctx, index, "type", type, sizeof(type))) {
            type[0] = '\0';
        }

        const bool data = dataspeedSysfsTypeIs_(type, "Data");
        const bool instruction = dataspeedSysfsTypeIs_(type, "Instruction");

        size_t size = 0;
        const bool hasSize = read(ctx, index, "size", buf, sizeof(buf)) &&
                             dataspeedParseCacheSize_(buf, &size);

        uint64_t line = 0;
        const bool hasLine =
            read(ctx, index, "coherency_line_size", buf, sizeof(buf)) &&
            dataspeedParseSysfsUInt_(buf, DATASPEED_MAX_LINE_SIZE, &line) &&
            line > 0;
        if (hasLine && firstLine == 0) {
            firstLine = (size_t)line;
        }

        if (level == 1) {
            if (instruction) {
                if (hasSize) {
                    cache->l1i_size = size;
                }
                l1iDedicated = true;
            } else if (data) {
                if (hasSize) {
                    cache->l1d_size = size;
                }
                if (hasLine) {
                    l1dLine = (size_t)line;
                }
                l1dDedicated = true;
            } else {
                /* Unified, or of unknown type */
                if (!l1dDedicated && hasSize) {
                    cache->l1d_size = size;
                }
                if (!l1dDedicated && hasLine) {
                    l1dLine = (size_t)line;
                }
                if (!l1iDedicated && hasSize) {
                    cache->l1i_size = size;
                }
            }
        } else if (hasSize && !instruction) {
            if (level == 2) {
                cache->l2_size = size;
            } else if (level == 3) {
                cache->l3_size = size;
            }
        }
    }

    cache->line_size = l1dLine     ? l1dLine
                       : firstLine ? firstLine
                                   : DATASPEED_DEFAULT_LINE_SIZE;
}

/* Decode Apple's hw.* cache sysctls into 'cache'.  Missing or nonpositive
 * values stay 0 (unknown); a missing or out-of-range line size is
 * DATASPEED_DEFAULT_LINE_SIZE. */
DK_FN_UNUSED static void dataspeedCacheFromSysctl_(dataspeedCacheInfo *cache,
                                                   dataspeedSysctlReadFn read,
                                                   void *ctx) {
    const struct {
        const char *name;
        size_t *dst;
    } sizes[] = {
        {"hw.l1dcachesize", &cache->l1d_size},
        {"hw.l1icachesize", &cache->l1i_size},
        {"hw.l2cachesize", &cache->l2_size},
        {"hw.l3cachesize", &cache->l3_size},
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        int64_t value = 0;
        if (read(ctx, sizes[i].name, &value) && value > 0) {
            *sizes[i].dst = (size_t)value;
        }
    }

    int64_t line = 0;
    const bool lineOk = read(ctx, "hw.cachelinesize", &line) && line > 0 &&
                        line <= DATASPEED_MAX_LINE_SIZE;
    cache->line_size = lineOk ? (size_t)line : DATASPEED_DEFAULT_LINE_SIZE;
}

#if defined(__linux__)
static bool dataspeedSysfsReadFile_(void *ctx, int32_t index, const char *attr,
                                    char *buf, size_t len) {
    (void)ctx;
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%" PRId32 "/%s", index,
             attr);
    return dataspeedReadLine_(path, buf, len);
}
#endif

#if defined(__APPLE__)
/* hw.* integer sysctls are int64_t, but accept a 32-bit one too: reading
 * an int64_t sysctl into a smaller buffer fails outright */
static bool dataspeedSysctlReadInt_(void *ctx, const char *name,
                                    int64_t *value) {
    (void)ctx;
    union {
        int64_t i64;
        int32_t i32;
    } got = {};
    size_t len = sizeof(got);
    if (sysctlbyname(name, &got, &len, NULL, 0) != 0) {
        return false;
    }

    if (len == sizeof(int64_t)) {
        *value = got.i64;
    } else if (len == sizeof(int32_t)) {
        *value = got.i32;
    } else {
        return false;
    }

    return true;
}
#endif

/* ============================================================================
 * System Information Detection
 * ============================================================================
 */

void dataspeedGetSystemInfo(dataspeedSystemInfo *info) {
    memset(info, 0, sizeof(*info));

#if defined(__APPLE__)
    /* CPU model */
    size_t len = sizeof(info->cpu_model);
    sysctlbyname("machdep.cpu.brand_string", info->cpu_model, &len, NULL, 0);

    /* CPU count */
    int cpu_count = 0;
    len = sizeof(cpu_count);
    sysctlbyname("hw.ncpu", &cpu_count, &len, NULL, 0);
    info->cpu_count = cpu_count > 0 ? cpu_count : 1;

    /* Cache sizes and line size */
    dataspeedCacheFromSysctl_(&info->cache, dataspeedSysctlReadInt_, NULL);

    /* Total memory */
    int64_t memsize = 0;
    len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    info->total_memory = memsize;

#elifdef __linux__
    /* CPU model from /proc/cpuinfo */
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ') {
                        colon++;
                    }
                    char *nl = strchr(colon, '\n');
                    if (nl) {
                        *nl = '\0';
                    }
                    strncpy(info->cpu_model, colon,
                            sizeof(info->cpu_model) - 1);
                }
                break;
            }
        }
        fclose(fp);
    }

    /* CPU count (sysconf returns -1 when it cannot tell) */
    const int64_t onlineCpus = sysconf(_SC_NPROCESSORS_ONLN);
    info->cpu_count = onlineCpus > 0 ? (size_t)onlineCpus : 1;

    /* Cache sizes and line size from sysfs */
    dataspeedCacheFromSysfs_(&info->cache, dataspeedSysfsReadFile_, NULL);

    /* Total memory */
    info->total_memory =
        (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);
#else
    strncpy(info->cpu_model, "Unknown", sizeof(info->cpu_model));
    info->cpu_count = 1;
    info->cache.line_size = 64;
#endif

    /* Page size (portable) */
    info->page_size = sysconf(_SC_PAGESIZE);

    /* Estimate CPU frequency via timing loop
     * Note: On ARM64 (Apple Silicon), CNTVCT_EL0 runs at a fixed frequency
     * (typically 24 MHz), not the CPU frequency. We use a different approach.
     */
#if defined(__APPLE__) && defined(__aarch64__)
    /* On Apple Silicon, try to get the actual CPU frequency from sysctl */
    uint64_t freq = 0;
    size_t freq_len = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency", &freq, &freq_len, NULL, 0) == 0) {
        info->cpu_freq_mhz = (double)freq / 1e6;
    } else if (sysctlbyname("hw.cpufrequency_max", &freq, &freq_len, NULL, 0) ==
               0) {
        info->cpu_freq_mhz = (double)freq / 1e6;
    } else {
        /* Fallback: estimate via actual computation throughput */
        volatile uint64_t dummy = 0;
        uint64_t start_ns = dataspeed_time_ns();
        for (int i = 0; i < 100'000'000; i++) {
            dummy += i;
        }
        uint64_t elapsed_ns = dataspeed_time_ns() - start_ns;
        (void)dummy;
        /* Rough estimate: ~1 add per cycle */
        info->cpu_freq_mhz = 100'000'000.0 / elapsed_ns * 1000.0;
    }
#else
    /* On x86, RDTSC runs at a fixed rate close to base frequency */
    uint64_t start_tsc = dataspeed_rdtsc();
    uint64_t start_ns = dataspeed_time_ns();

    /* Busy loop for ~10ms */
    volatile uint64_t dummy = 0;
    uint64_t target_ns = start_ns + 10'000'000; /* 10ms */
    while (dataspeed_time_ns() < target_ns) {
        dummy++;
    }
    (void)dummy;

    uint64_t end_tsc = dataspeed_rdtsc();
    uint64_t end_ns = dataspeed_time_ns();

    double elapsed_s = (end_ns - start_ns) / 1e9;
    double cycles = (double)(end_tsc - start_tsc);
    info->cpu_freq_mhz = (cycles / elapsed_s) / 1e6;
#endif
}

void dataspeedPrintSystemInfo(const dataspeedSystemInfo *info) {
    char buf[64];

    printf("=== System Information ===\n");
    printf("CPU:          %s\n", info->cpu_model);
    printf("CPU Count:    %zu logical cores\n", info->cpu_count);
    printf("CPU Freq:     %.0f MHz (estimated)\n", info->cpu_freq_mhz);
    printf("L1D Cache:    %s\n",
           format_bytes(info->cache.l1d_size, buf, sizeof(buf)));
    printf("L2 Cache:     %s\n",
           format_bytes(info->cache.l2_size, buf, sizeof(buf)));
    printf("L3 Cache:     %s\n",
           format_bytes(info->cache.l3_size, buf, sizeof(buf)));
    printf("Cache Line:   %zu bytes\n", info->cache.line_size);
    printf("Total Memory: %s\n",
           format_bytes(info->total_memory, buf, sizeof(buf)));
    printf("Page Size:    %zu bytes\n", info->page_size);
    printf("\n");
}

/* ============================================================================
 * Memory Bandwidth Benchmark
 * ============================================================================
 */

/* Prevent compiler from optimizing away reads */
static volatile uint64_t sink;

/* Simple bandwidth functions using libc memcpy/memset for accurate measurement
 */
DATASPEED_NOINLINE
static void bandwidth_read_opt(const uint64_t *data, size_t count) {
    /* Read by copying to a temp buffer - memcpy is highly optimized */
    static uint64_t temp[64]; /* Small buffer to copy into */
    size_t bytes = count * sizeof(uint64_t);
    size_t chunk = sizeof(temp);
    const char *src = (const char *)data;

    for (size_t i = 0; i < bytes; i += chunk) {
        size_t n = (bytes - i < chunk) ? (bytes - i) : chunk;
        memcpy(temp, src + i, n);
    }
    sink = temp[0]; /* Prevent optimization */
}

DATASPEED_NOINLINE
static void bandwidth_write_opt(uint64_t *data, size_t count, uint64_t val) {
    (void)val;
    memset(data, 0xAB, count * sizeof(uint64_t));
}

DATASPEED_NOINLINE
static void bandwidth_copy_opt(uint64_t *dst, const uint64_t *src,
                               size_t count) {
    memcpy(dst, src, count * sizeof(uint64_t));
}

/* Note: memcpy/memset already use SIMD internally on modern systems,
 * so there's no need for separate SIMD bandwidth measurements. */

static void benchmark_bandwidth(const char *name, size_t size_bytes,
                                void (*func)(uint64_t *, size_t, uint64_t *),
                                dataspeedBandwidthResult *result,
                                bool verbose) {
    const size_t count = size_bytes / sizeof(uint64_t);
    uint64_t *data = NULL;
    uint64_t *data2 = NULL; /* For copy */
    int alignRc = zmemalign((void **)&data, 64, size_bytes);
    alignRc |= zmemalign((void **)&data2, 64, size_bytes);
    assert(alignRc == 0 && data && data2);
    (void)alignRc;

    /* Initialize to avoid lazy allocation */
    memset(data, 0xAB, size_bytes);
    memset(data2, 0xCD, size_bytes);

    /* Warmup */
    for (int i = 0; i < 3; i++) {
        func(data, count, data2);
    }

    /* Collect samples */
    const size_t max_samples = 100;
    double samples[100];
    size_t sample_count = 0;

    uint64_t total_start = dataspeed_time_ns();
    uint64_t min_duration_ns = 500000000; /* 500ms */

    while (sample_count < max_samples) {
        TIMING_BARRIER(); /* Ensure timing is accurate */
        uint64_t start = dataspeed_time_ns();
        uint64_t start_tsc = dataspeed_rdtsc();
        TIMING_BARRIER();

        func(data, count, data2);

        TIMING_BARRIER();
        uint64_t end_tsc = dataspeed_rdtsc();
        uint64_t end = dataspeed_time_ns();
        TIMING_BARRIER();

        double elapsed_s = (end - start) / 1e9;
        samples[sample_count++] = size_bytes / elapsed_s / 1e9; /* GB/s */

        /* Store cycles for last sample */
        result->cycles_per_byte = (double)(end_tsc - start_tsc) / size_bytes;

        if (dataspeed_time_ns() - total_start > min_duration_ns &&
            sample_count >= 10) {
            break;
        }
    }

    compute_stats(samples, sample_count, &result->stats);
    result->bandwidth_gbs = result->stats.median;

    if (verbose) {
        printf("  %-20s %6.2f GB/s (%.4f cycles/byte)\n", name,
               result->bandwidth_gbs, result->cycles_per_byte);
    }

    zfree(data);
    zfree(data2);
}

/* Wrapper functions for benchmark_bandwidth */
static void bw_read(uint64_t *d, size_t c, uint64_t *d2) {
    (void)d2;
    bandwidth_read_opt(d, c);
}
static void bw_write(uint64_t *d, size_t c, uint64_t *d2) {
    (void)d2;
    bandwidth_write_opt(d, c, 0xDEADBEEF);
}
static void bw_copy(uint64_t *d, size_t c, uint64_t *d2) {
    bandwidth_copy_opt(d, d2, c);
}

void dataspeedBenchmarkMemory(dataspeedMemoryResults *results, size_t size_mb) {
    memset(results, 0, sizeof(*results));

    size_t size_bytes = size_mb * (1ULL << 20);
    bool verbose = true;

    printf("=== Memory Bandwidth (%zu MB working set) ===\n", size_mb);

    benchmark_bandwidth("Sequential Read", size_bytes, bw_read,
                        &results->seq_read, verbose);
    benchmark_bandwidth("Sequential Write", size_bytes, bw_write,
                        &results->seq_write, verbose);
    benchmark_bandwidth("Sequential Copy", size_bytes, bw_copy,
                        &results->seq_copy, verbose);

    printf("\n");
}

/* ============================================================================
 * Memory Latency Benchmark (Pointer Chasing)
 * ============================================================================
 */

/* Create a shuffled pointer chain for true latency measurement */
static void **create_pointer_chain(size_t size_bytes) {
    size_t count = size_bytes / sizeof(void *);
    void **chain = NULL;
    int alignRc = zmemalign((void **)&chain, 64, size_bytes);
    assert(alignRc == 0 && chain);
    (void)alignRc;

    /* Create indices and shuffle (Fisher-Yates) */
    cleanerName(zfree, Sizes) size_t *indices = zmalloc(count * sizeof(size_t));
    if (!indices) {
        zfree(chain);
        return NULL; /* Memory allocation failed */
    }
    for (size_t i = 0; i < count; i++) {
        indices[i] = i;
    }
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    /* Build chain */
    for (size_t i = 0; i < count - 1; i++) {
        chain[indices[i]] = &chain[indices[i + 1]];
    }
    chain[indices[count - 1]] = &chain[indices[0]]; /* Circular */

    return chain;
}

/* Pointer chasing - critical that each load depends on the previous */
DATASPEED_BENCHMARK_FUNC
static size_t chase_pointers(void **start, size_t iterations) {
    void **p = start;
    ESCAPE(p);
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        p = (void **)*p;
        /* Critical: force the dependency chain to be honored */
        DO_NOT_OPTIMIZE(p);
    }
    COMPILER_BARRIER();
    return (size_t)p; /* Return value prevents optimization */
}

static void benchmark_latency_at_size(size_t size_bytes,
                                      dataspeedLatencyResult *result,
                                      bool verbose) {
    void **chain = create_pointer_chain(size_bytes);

    /* Warmup */
    chase_pointers(chain, 10000);

    /* Determine iteration count for ~100ms */
    size_t iterations = 100000;
    uint64_t start = dataspeed_time_ns();
    chase_pointers(chain, iterations);
    uint64_t elapsed = dataspeed_time_ns() - start;

    /* Scale to ~100ms */
    iterations = (size_t)((double)iterations * 100'000'000.0 / elapsed);
    if (iterations < 10000) {
        iterations = 10000;
    }

    /* Collect samples */
    const size_t max_samples = 20;
    double samples[20];

    for (size_t s = 0; s < max_samples; s++) {
        TIMING_BARRIER();
        uint64_t start_ns = dataspeed_time_ns();
        uint64_t start_tsc = dataspeed_rdtsc();
        TIMING_BARRIER();

        size_t dummy = chase_pointers(chain, iterations);
        DO_NOT_OPTIMIZE(dummy);

        TIMING_BARRIER();
        uint64_t end_tsc = dataspeed_rdtsc();
        uint64_t end_ns = dataspeed_time_ns();
        TIMING_BARRIER();

        samples[s] = (double)(end_ns - start_ns) / iterations;
        result->cycles = (double)(end_tsc - start_tsc) / iterations;
    }

    compute_stats(samples, max_samples, &result->stats);
    result->latency_ns = result->stats.median;

    char buf[32];
    if (verbose) {
        printf("  %-12s %6.2f ns (%5.1f cycles)\n",
               format_bytes(size_bytes, buf, sizeof(buf)), result->latency_ns,
               result->cycles);
    }

    zfree(chain);
}

void dataspeedBenchmarkLatency(dataspeedLatencyResults *results) {
    memset(results, 0, sizeof(*results));

    printf("=== Memory Latency (pointer chasing) ===\n");

    /* Test at sizes targeting each cache level */
    benchmark_latency_at_size(32 * 1024, &results->l1_latency,
                              true); /* 32 KB - L1 */
    benchmark_latency_at_size(256 * 1024, &results->l2_latency,
                              true); /* 256 KB - L2 */
    benchmark_latency_at_size(8 * 1024 * 1024, &results->l3_latency,
                              true); /* 8 MB - L3 */
    benchmark_latency_at_size(64 * 1024 * 1024, &results->mem_latency,
                              true); /* 64 MB - RAM */

    printf("\n");
}

/* ============================================================================
 * Cache Hierarchy Benchmark
 * ============================================================================
 */

void dataspeedBenchmarkCacheHierarchy(dataspeedCacheResults *results) {
    memset(results, 0, sizeof(*results));

    printf("=== Cache Bandwidth by Level ===\n");

    /* Test bandwidth at different sizes to see cache effects */
    size_t sizes[] = {
        32 * 1024,       /* L1 (~32KB) */
        256 * 1024,      /* L2 (~256KB) */
        4 * 1024 * 1024, /* L3 (~4MB per core) */
        64 * 1024 * 1024 /* Main memory */
    };

    dataspeedBandwidthResult *targets[] = {
        &results->l1_bandwidth, &results->l2_bandwidth, &results->l3_bandwidth,
        &results->mem_bandwidth};

    const char *names[] = {"L1 (32KB)", "L2 (256KB)", "L3 (4MB)", "RAM (64MB)"};

    for (int i = 0; i < 4; i++) {
        benchmark_bandwidth(names[i], sizes[i], bw_read, targets[i], true);
    }

    printf("\n");
}

/* ============================================================================
 * CPU Operations Benchmark
 * ============================================================================
 */

/* CPU throughput benchmarks - optnone prevents optimization while
 * maintaining realistic loop overhead. Each iteration has data dependencies
 * that create realistic pipeline behavior. */
DATASPEED_BENCHMARK_FUNC
static uint64_t bench_int_add(size_t iterations) {
    uint64_t a = 1, b = 2, c = 3, d = 4;
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        a += b;
        b += c;
        c += d;
        d += a;
        a += b;
        b += c;
        c += d;
        d += a;
    }
    COMPILER_BARRIER();
    uint64_t result = a + b + c + d;
    DO_NOT_OPTIMIZE(result);
    return result;
}

DATASPEED_BENCHMARK_FUNC
static uint64_t bench_int_mul(size_t iterations) {
    uint64_t a = 3, b = 5, c = 7, d = 11;
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        a *= b;
        b *= c;
        c *= d;
        d *= a;
    }
    COMPILER_BARRIER();
    uint64_t result = a + b + c + d;
    DO_NOT_OPTIMIZE(result);
    return result;
}

DATASPEED_BENCHMARK_FUNC
static double bench_float_add(size_t iterations) {
    double a = 1.1, b = 2.2, c = 3.3, d = 4.4;
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        a += b;
        b += c;
        c += d;
        d += a;
        a += b;
        b += c;
        c += d;
        d += a;
    }
    COMPILER_BARRIER();
    double result = a + b + c + d;
    DO_NOT_OPTIMIZE(result);
    return result;
}

DATASPEED_BENCHMARK_FUNC
static double bench_float_mul(size_t iterations) {
    double a = 1.0001, b = 1.0002, c = 1.0003, d = 1.0004;
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        a *= b;
        b *= c;
        c *= d;
        d *= a;
    }
    COMPILER_BARRIER();
    double result = a + b + c + d;
    DO_NOT_OPTIMIZE(result);
    return result;
}

DATASPEED_BENCHMARK_FUNC
static double bench_float_fma(size_t iterations) {
    double a = 1.1, b = 2.2, c = 3.3, d = 4.4;
    double e = 1.01;
    COMPILER_BARRIER();
    for (size_t i = 0; i < iterations; i++) {
        a = a * e + b;
        b = b * e + c;
        c = c * e + d;
        d = d * e + a;
    }
    COMPILER_BARRIER();
    double result = a + b + c + d;
    DO_NOT_OPTIMIZE(result);
    return result;
}

/* Function call overhead measurement */
DATASPEED_BENCHMARK_FUNC
static size_t dummy_call(void) {
    COMPILER_BARRIER();
    return 42;
}

void dataspeedBenchmarkCPU(dataspeedCPUResults *results) {
    memset(results, 0, sizeof(*results));

    printf("=== CPU Operations ===\n");

    const size_t iterations = 100'000'000; /* 100M iterations */
    uint64_t start, elapsed;
    uint64_t r1;
    double r3, r4, r5;

    /* Integer add (8 ops per iteration) */
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    r1 = bench_int_add(iterations);
    DO_NOT_OPTIMIZE(r1);
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->int_add_gops = (iterations * 8.0) / elapsed; /* ops/ns = Gops */
    printf("  Int Add:     %6.2f Gops/s\n", results->int_add_gops);

    /* Integer mul (4 ops per iteration) */
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    r1 = bench_int_mul(iterations / 10);
    DO_NOT_OPTIMIZE(r1);
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->int_mul_gops = ((iterations / 10) * 4.0) / elapsed;
    printf("  Int Mul:     %6.2f Gops/s\n", results->int_mul_gops);

    /* Float add (8 ops per iteration) */
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    r3 = bench_float_add(iterations);
    DO_NOT_OPTIMIZE(r3);
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->float_add_gops = (iterations * 8.0) / elapsed;
    printf("  Float Add:   %6.2f Gops/s\n", results->float_add_gops);

    /* Float mul (4 ops per iteration) */
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    r4 = bench_float_mul(iterations / 10);
    DO_NOT_OPTIMIZE(r4);
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->float_mul_gops = ((iterations / 10) * 4.0) / elapsed;
    printf("  Float Mul:   %6.2f Gops/s\n", results->float_mul_gops);

    /* FMA (4 ops per iteration) */
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    r5 = bench_float_fma(iterations);
    DO_NOT_OPTIMIZE(r5);
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->float_fma_gops = (iterations * 4.0) / elapsed;
    printf("  Float FMA:   %6.2f Gops/s\n", results->float_fma_gops);

    /* Function call overhead */
    size_t call_iters = 10'000'000;
    TIMING_BARRIER();
    start = dataspeed_time_ns();
    TIMING_BARRIER();
    for (size_t i = 0; i < call_iters; i++) {
        size_t x = dummy_call();
        DO_NOT_OPTIMIZE(x);
    }
    TIMING_BARRIER();
    elapsed = dataspeed_time_ns() - start;
    TIMING_BARRIER();
    results->call_overhead_ns = (double)elapsed / call_iters;
    printf("  Call Overhead: %.2f ns\n", results->call_overhead_ns);

    printf("\n");
}

/* ============================================================================
 * Comprehensive Report
 * ============================================================================
 */

void dataspeedRunAll(dataspeedReport *report, bool verbose) {
    memset(report, 0, sizeof(*report));

    uint64_t start = dataspeed_time_ns();
    report->timestamp = (uint64_t)time(NULL);

    /* System info */
    dataspeedGetSystemInfo(&report->system);
    if (verbose) {
        dataspeedPrintSystemInfo(&report->system);
    }

    /* CPU benchmark */
    dataspeedBenchmarkCPU(&report->cpu);

    /* Cache hierarchy */
    dataspeedBenchmarkCacheHierarchy(&report->cache);

    /* Memory latency */
    dataspeedBenchmarkLatency(&report->latency);

    /* Memory bandwidth (use 64MB to exceed L3) */
    dataspeedBenchmarkMemory(&report->memory, 64);

    report->total_duration_s = (dataspeed_time_ns() - start) / 1e9;

    printf("=== Benchmark Complete (%.1f seconds) ===\n",
           report->total_duration_s);
}

/* ============================================================================
 * Report Formatting Utilities
 * ============================================================================
 */

#define REPORT_WIDTH 70

/* Box drawing characters (UTF-8) */
#define BOX_TL "\xe2\x95\x94" /* ╔ */
#define BOX_TR "\xe2\x95\x97" /* ╗ */
#define BOX_BL "\xe2\x95\x9a" /* ╚ */
#define BOX_BR "\xe2\x95\x9d" /* ╝ */
#define BOX_H "\xe2\x95\x90"  /* ═ */
#define BOX_V "\xe2\x95\x91"  /* ║ */
#define BOX_ML "\xe2\x95\xa0" /* ╠ */
#define BOX_MR "\xe2\x95\xa3" /* ╣ */

/* Print horizontal separator line */
static void report_hline(const char *left, const char *right) {
    printf("%s", left);
    for (int i = 0; i < REPORT_WIDTH - 2; i++) {
        printf("%s", BOX_H);
    }
    printf("%s\n", right);
}

/* Print a content row with proper alignment */
static void report_row(const char *fmt, ...) {
    char content[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(content, sizeof(content), fmt, args);
    va_end(args);

    /* Calculate visible width (UTF-8 aware) */
    size_t visible_len = 0;
    const unsigned char *p = (const unsigned char *)content;
    while (*p) {
        if ((*p & 0xC0) != 0x80) {
            visible_len++;
        }
        p++;
    }

    int padding = REPORT_WIDTH - 4 - (int)visible_len;
    if (padding < 0) {
        padding = 0;
    }

    printf("%s %s%*s %s\n", BOX_V, content, padding, "", BOX_V);
}

/* Format helpers */
static const char *fmt_bw(double gbs, char *buf, size_t len) {
    snprintf(buf, len, "%6.2f GB/s", gbs);
    return buf;
}

static const char *fmt_lat(double ns, char *buf, size_t len) {
    if (ns >= 1000.0) {
        snprintf(buf, len, "%7.2f us", ns / 1000.0);
    } else {
        snprintf(buf, len, "%7.2f ns", ns);
    }
    return buf;
}

static const char *fmt_ops(double gops, char *buf, size_t len) {
    snprintf(buf, len, "%6.2f Gops/s", gops);
    return buf;
}

void dataspeedPrintReport(const dataspeedReport *report) {
    char b1[32], b2[32], b3[32];

    printf("\n");
    report_hline(BOX_TL, BOX_TR);
    report_row("              DATASPEED BENCHMARK REPORT");
    report_hline(BOX_ML, BOX_MR);

    /* System Info */
    report_row("%s", report->system.cpu_model);
    report_row("Cores: %-3zu   Freq: %-7.0f MHz   RAM: %.1f GB",
               report->system.cpu_count, report->system.cpu_freq_mhz,
               report->system.total_memory / 1e9);
    if (report->system.cache.l1d_size > 0) {
        report_row("L1D: %-4zuKB  L2: %-5zuKB  L3: %-5zuKB  Line: %zuB",
                   report->system.cache.l1d_size / 1024,
                   report->system.cache.l2_size / 1024,
                   report->system.cache.l3_size / 1024,
                   report->system.cache.line_size);
    }

    /* Cache Bandwidth */
    report_hline(BOX_ML, BOX_MR);
    report_row("CACHE BANDWIDTH");
    report_row(
        "  L1: %s   L2: %s",
        fmt_bw(report->cache.l1_bandwidth.bandwidth_gbs, b1, sizeof(b1)),
        fmt_bw(report->cache.l2_bandwidth.bandwidth_gbs, b2, sizeof(b2)));
    report_row(
        "  L3: %s   RAM: %s",
        fmt_bw(report->cache.l3_bandwidth.bandwidth_gbs, b1, sizeof(b1)),
        fmt_bw(report->cache.mem_bandwidth.bandwidth_gbs, b2, sizeof(b2)));

    /* Memory Latency */
    report_hline(BOX_ML, BOX_MR);
    report_row("MEMORY LATENCY");
    report_row("  L1: %s   L2: %s",
               fmt_lat(report->latency.l1_latency.latency_ns, b1, sizeof(b1)),
               fmt_lat(report->latency.l2_latency.latency_ns, b2, sizeof(b2)));
    report_row("  L3: %s   RAM: %s",
               fmt_lat(report->latency.l3_latency.latency_ns, b1, sizeof(b1)),
               fmt_lat(report->latency.mem_latency.latency_ns, b2, sizeof(b2)));

    /* Memory Bandwidth */
    report_hline(BOX_ML, BOX_MR);
    report_row("MEMORY BANDWIDTH (64 MB working set)");
    report_row("  Read: %s   Write: %s   Copy: %s",
               fmt_bw(report->memory.seq_read.bandwidth_gbs, b1, sizeof(b1)),
               fmt_bw(report->memory.seq_write.bandwidth_gbs, b2, sizeof(b2)),
               fmt_bw(report->memory.seq_copy.bandwidth_gbs, b3, sizeof(b3)));

    /* CPU Throughput */
    report_hline(BOX_ML, BOX_MR);
    report_row("CPU THROUGHPUT");
    report_row("  Int Add: %s   Int Mul: %s",
               fmt_ops(report->cpu.int_add_gops, b1, sizeof(b1)),
               fmt_ops(report->cpu.int_mul_gops, b2, sizeof(b2)));
    report_row("  FP Add:  %s   FP Mul:  %s",
               fmt_ops(report->cpu.float_add_gops, b1, sizeof(b1)),
               fmt_ops(report->cpu.float_mul_gops, b2, sizeof(b2)));
    report_row("  FP FMA:  %s   Call:    %s",
               fmt_ops(report->cpu.float_fma_gops, b1, sizeof(b1)),
               fmt_lat(report->cpu.call_overhead_ns, b2, sizeof(b2)));

    /* Footer */
    report_hline(BOX_ML, BOX_MR);
    report_row("Benchmark completed in %.1f seconds", report->total_duration_s);
    report_hline(BOX_BL, BOX_BR);
}

void dataspeedPrintReportCSV(const dataspeedReport *report) {
    printf("metric,value,unit\n");
    printf("cpu_freq_mhz,%.0f,MHz\n", report->system.cpu_freq_mhz);
    printf("cpu_cores,%zu,count\n", report->system.cpu_count);
    printf("l1_bandwidth,%.2f,GB/s\n",
           report->cache.l1_bandwidth.bandwidth_gbs);
    printf("l2_bandwidth,%.2f,GB/s\n",
           report->cache.l2_bandwidth.bandwidth_gbs);
    printf("l3_bandwidth,%.2f,GB/s\n",
           report->cache.l3_bandwidth.bandwidth_gbs);
    printf("mem_bandwidth,%.2f,GB/s\n",
           report->cache.mem_bandwidth.bandwidth_gbs);
    printf("l1_latency,%.2f,ns\n", report->latency.l1_latency.latency_ns);
    printf("l2_latency,%.2f,ns\n", report->latency.l2_latency.latency_ns);
    printf("l3_latency,%.2f,ns\n", report->latency.l3_latency.latency_ns);
    printf("mem_latency,%.2f,ns\n", report->latency.mem_latency.latency_ns);
    printf("seq_read,%.2f,GB/s\n", report->memory.seq_read.bandwidth_gbs);
    printf("seq_write,%.2f,GB/s\n", report->memory.seq_write.bandwidth_gbs);
    printf("int_add,%.2f,Gops\n", report->cpu.int_add_gops);
    printf("float_add,%.2f,Gops\n", report->cpu.float_add_gops);
}

/* Helper to add a double as JSON */
static void dj_double(djState *dj, const char *key, double val) {
    char buf[32];
    size_t len = StrDoubleFormatToBufNice(buf, sizeof(buf), val);
    djStringDirect(dj, key, strlen(key));
    djNumericDirect(dj, buf, len);
}

/* Helper to add a size_t as JSON */
static void dj_size(djState *dj, const char *key, size_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%zu", val);
    djStringDirect(dj, key, strlen(key));
    djNumericDirect(dj, buf, (size_t)len);
}

/* Helper to add a string as JSON */
static void dj_str(djState *dj, const char *key, const char *val) {
    djStringDirect(dj, key, strlen(key));
    djString(dj, val, strlen(val), true);
}

static mds *dataspeedReportToJSON(const dataspeedReport *report) {
    djState dj = {}; /* Must zero-init before djInit */
    djInit(&dj);

    djMapOpen(&dj);

    /* System info */
    djStringDirect(&dj, "system", 6);
    djMapOpen(&dj);
    dj_str(&dj, "cpu_model", report->system.cpu_model);
    dj_size(&dj, "cpu_count", report->system.cpu_count);
    dj_double(&dj, "cpu_freq_mhz", report->system.cpu_freq_mhz);
    dj_size(&dj, "total_memory_bytes", report->system.total_memory);
    dj_size(&dj, "page_size", report->system.page_size);

    djStringDirect(&dj, "cache", 5);
    djMapOpen(&dj);
    dj_size(&dj, "l1d_size", report->system.cache.l1d_size);
    dj_size(&dj, "l1i_size", report->system.cache.l1i_size);
    dj_size(&dj, "l2_size", report->system.cache.l2_size);
    dj_size(&dj, "l3_size", report->system.cache.l3_size);
    dj_size(&dj, "line_size", report->system.cache.line_size);
    djMapCloseElement(&dj); /* cache */
    djMapCloseElement(&dj); /* system */

    /* Cache bandwidth */
    djStringDirect(&dj, "cache_bandwidth", 15);
    djMapOpen(&dj);
    dj_double(&dj, "l1_gbs", report->cache.l1_bandwidth.bandwidth_gbs);
    dj_double(&dj, "l2_gbs", report->cache.l2_bandwidth.bandwidth_gbs);
    dj_double(&dj, "l3_gbs", report->cache.l3_bandwidth.bandwidth_gbs);
    dj_double(&dj, "ram_gbs", report->cache.mem_bandwidth.bandwidth_gbs);
    djMapCloseElement(&dj);

    /* Memory latency */
    djStringDirect(&dj, "memory_latency", 14);
    djMapOpen(&dj);
    dj_double(&dj, "l1_ns", report->latency.l1_latency.latency_ns);
    dj_double(&dj, "l2_ns", report->latency.l2_latency.latency_ns);
    dj_double(&dj, "l3_ns", report->latency.l3_latency.latency_ns);
    dj_double(&dj, "ram_ns", report->latency.mem_latency.latency_ns);
    djMapCloseElement(&dj);

    /* Memory bandwidth */
    djStringDirect(&dj, "memory_bandwidth", 16);
    djMapOpen(&dj);
    dj_double(&dj, "seq_read_gbs", report->memory.seq_read.bandwidth_gbs);
    dj_double(&dj, "seq_write_gbs", report->memory.seq_write.bandwidth_gbs);
    dj_double(&dj, "seq_copy_gbs", report->memory.seq_copy.bandwidth_gbs);
    djMapCloseElement(&dj);

    /* CPU throughput */
    djStringDirect(&dj, "cpu_throughput", 14);
    djMapOpen(&dj);
    dj_double(&dj, "int_add_gops", report->cpu.int_add_gops);
    dj_double(&dj, "int_mul_gops", report->cpu.int_mul_gops);
    dj_double(&dj, "float_add_gops", report->cpu.float_add_gops);
    dj_double(&dj, "float_mul_gops", report->cpu.float_mul_gops);
    dj_double(&dj, "float_fma_gops", report->cpu.float_fma_gops);
    dj_double(&dj, "call_overhead_ns", report->cpu.call_overhead_ns);
    djMapCloseElement(&dj);

    /* Metadata */
    dj_size(&dj, "timestamp", (size_t)report->timestamp);
    dj_double(&dj, "duration_seconds", report->total_duration_s);

    djMapCloseFinal(&dj);

    return djFinalize(&dj);
}

void dataspeedPrintReportJSON(const dataspeedReport *report) {
    mds *json = dataspeedReportToJSON(report);
    printf("%s\n", json);
    mdsfree(json);
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

dataspeedConfig dataspeedDefaultConfig(void) {
    return (dataspeedConfig){
        .min_duration_ms = 100,
        .max_iterations = 1'000'000,
        .warmup_iterations = 10,
        .verbose = true,
        .include_latency = true,
    };
}

void dataspeedRunWithConfig(dataspeedReport *report,
                            const dataspeedConfig *config) {
    (void)config; /* TODO: Use config to customize benchmarks */
    dataspeedRunAll(report, config->verbose);
}

/* ============================================================================
 * Original API (preserved for compatibility)
 * ============================================================================
 */

static sig_atomic_t stopProcessing = false;

static void doStop(int signal) {
    (void)signal;
    stopProcessing = true;
}

DATASPEED_NO_OPTIMIZE size_t dataspeed(double MB, size_t iterations) {
    const sig_t prev = signal(SIGINT, doStop);

    printf("=== DATASPEED - System Performance Benchmark ===\n\n");

    /* Run comprehensive benchmark */
    dataspeedReport report;
    dataspeedRunAll(&report, true);

    /* Also run the legacy memory test if requested */
    if (MB > 0 && iterations > 0) {
        printf("=== Legacy Memory Test (%.0f MB x %zu iterations) ===\n", MB,
               iterations);

        size_t sizeBytes = (size_t)(MB * (1 << 20));
        cleanerName(zfree, Chars) char *mem = zmalloc(sizeBytes);
        assert(mem);

        uint64_t totalStart = dataspeed_time_ns();

        for (size_t i = 0; i < iterations && !stopProcessing; i++) {
            memset(mem, 0xDA, sizeBytes);
            if (i && i % 50 == 0) {
                uint64_t elapsed = dataspeed_time_ns() - totalStart;
                double gbs = (double)(i * sizeBytes) / elapsed;
                printf("  Progress: %zu/%zu (%.2f GB/s)\n", i, iterations, gbs);
            }
        }

        uint64_t totalElapsed = dataspeed_time_ns() - totalStart;
        double totalGB = (double)(iterations * sizeBytes) / (1ULL << 30);
        double seconds = totalElapsed / 1e9;
        printf("  Total: %.2f GB in %.2f seconds = %.2f GB/s\n", totalGB,
               seconds, totalGB / seconds);
    }

    /* Print summary report */
    dataspeedPrintReport(&report);

    signal(SIGINT, prev);
    return EXIT_SUCCESS;
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "ctest.h"

int dataspeedTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("System info detection") {
        dataspeedSystemInfo info;
        dataspeedGetSystemInfo(&info);

        /* Should detect some reasonable values */
        assert(info.cpu_count >= 1);
        assert(info.page_size > 0);
        assert(info.cpu_freq_mhz > 100); /* At least 100 MHz */
        assert(info.cache.line_size > 0);

        printf("  CPU: %s\n", info.cpu_model);
        printf("  Cores: %zu, Freq: %.0f MHz\n", info.cpu_count,
               info.cpu_freq_mhz);
    }

    TEST("Memory bandwidth measurement") {
        dataspeedMemoryResults results;
        dataspeedBenchmarkMemory(&results, 4); /* 4 MB - small for quick test */

        /* Should get reasonable bandwidth */
        assert(results.seq_read.bandwidth_gbs > 0.1);
        assert(results.seq_write.bandwidth_gbs > 0.1);
        assert(results.seq_copy.bandwidth_gbs > 0.1);
    }

    TEST("Memory latency measurement") {
        dataspeedLatencyResults results;
        dataspeedBenchmarkLatency(&results);

        /* L1 should be faster than RAM */
        assert(results.l1_latency.latency_ns < results.mem_latency.latency_ns);

        /* Should get reasonable latencies */
        assert(results.l1_latency.latency_ns > 0.1);
        assert(results.l1_latency.latency_ns < 100); /* L1 should be < 100ns */
    }

    TEST("CPU operations measurement") {
        dataspeedCPUResults results;
        dataspeedBenchmarkCPU(&results);

        /* Should get reasonable throughput */
        assert(results.int_add_gops > 0.1);
        assert(results.float_add_gops > 0.1);
        assert(results.call_overhead_ns > 0);
        assert(results.call_overhead_ns < 1000); /* Should be < 1us */
    }

    TEST_FINAL_RESULT;
}

/* A sample sysfs cache directory: one attribute value per row */
typedef struct dataspeedSysfsRow_ {
    int32_t index;
    const char *attr;
    const char *value;
} dataspeedSysfsRow_;

typedef struct dataspeedSysfsLayout_ {
    const dataspeedSysfsRow_ *rows;
    size_t count;
} dataspeedSysfsLayout_;

static bool dataspeedSysfsReadSample_(void *ctx, int32_t index,
                                      const char *attr, char *buf, size_t len) {
    const dataspeedSysfsLayout_ *layout = ctx;
    for (size_t i = 0; i < layout->count; i++) {
        if (layout->rows[i].index == index &&
            strcmp(layout->rows[i].attr, attr) == 0) {
            snprintf(buf, len, "%s", layout->rows[i].value);
            return true;
        }
    }

    return false;
}

/* A sample sysctl namespace: name and value per row */
typedef struct dataspeedSysctlRow_ {
    const char *name;
    int64_t value;
} dataspeedSysctlRow_;

typedef struct dataspeedSysctlLayout_ {
    const dataspeedSysctlRow_ *rows;
    size_t count;
} dataspeedSysctlLayout_;

static bool dataspeedSysctlReadSample_(void *ctx, const char *name,
                                       int64_t *value) {
    const dataspeedSysctlLayout_ *layout = ctx;
    for (size_t i = 0; i < layout->count; i++) {
        if (strcmp(layout->rows[i].name, name) == 0) {
            *value = layout->rows[i].value;
            return true;
        }
    }

    return false;
}

#define DS_LAYOUT_(rows) {(rows), sizeof(rows) / sizeof(*(rows))}

static int dataspeedCheckCache_(const char *what, const dataspeedCacheInfo *got,
                                const dataspeedCacheInfo *want) {
    int err = 0;
    if (got->l1d_size != want->l1d_size || got->l1i_size != want->l1i_size ||
        got->l2_size != want->l2_size || got->l3_size != want->l3_size ||
        got->line_size != want->line_size) {
        ERR("%s: got l1d %zu l1i %zu l2 %zu l3 %zu line %zu; want l1d %zu "
            "l1i %zu l2 %zu l3 %zu line %zu",
            what, got->l1d_size, got->l1i_size, got->l2_size, got->l3_size,
            got->line_size, want->l1d_size, want->l1i_size, want->l2_size,
            want->l3_size, want->line_size);
    }
    return err;
}

int dataspeedParseTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("sysfs number parsers") {
        size_t bytes = 7;
        assert(dataspeedParseCacheSize_("32K\n", &bytes) && bytes == 32768);
        assert(dataspeedParseCacheSize_("8M\n", &bytes) &&
               bytes == 8 * 1024 * 1024);
        assert(dataspeedParseCacheSize_("64", &bytes) && bytes == 64);
        bytes = 7;
        assert(!dataspeedParseCacheSize_("", &bytes) && bytes == 7);
        assert(!dataspeedParseCacheSize_("-1K\n", &bytes) && bytes == 7);
        assert(!dataspeedParseCacheSize_(" 32K\n", &bytes) && bytes == 7);
        assert(!dataspeedParseCacheSize_("32KB\n", &bytes) && bytes == 7);
        assert(!dataspeedParseCacheSize_("18014398509481984K", &bytes) &&
               bytes == 7);
        assert(!dataspeedParseCacheSize_("18446744073709551616\n", &bytes) &&
               bytes == 7);

        uint64_t v = 7;
        assert(dataspeedParseSysfsUInt_("64\n", 1 << 20, &v) && v == 64);
        assert(dataspeedParseSysfsUInt_("16", 16, &v) && v == 16);
        v = 7;
        assert(!dataspeedParseSysfsUInt_("17\n", 16, &v) && v == 7);
        assert(!dataspeedParseSysfsUInt_("-1\n", UINT64_MAX, &v) && v == 7);
        assert(!dataspeedParseSysfsUInt_("1x\n", 16, &v) && v == 7);
        assert(!dataspeedParseSysfsUInt_("\n", 16, &v) && v == 7);
        assert(
            !dataspeedParseSysfsUInt_("99999999999999999999", UINT64_MAX, &v) &&
            v == 7);
        (void)bytes;
        (void)v;
    }

    TEST("sysfs: L1 data size comes from the Data entry, not the last L1") {
        /* x86 order: index0 L1d, index1 L1i (read last, and larger) */
        static const dataspeedSysfsRow_ x86[] = {
            {0, "level", "1\n"},     {0, "type", "Data\n"},
            {0, "size", "48K\n"},    {0, "coherency_line_size", "64\n"},
            {1, "level", "1\n"},     {1, "type", "Instruction\n"},
            {1, "size", "32K\n"},    {1, "coherency_line_size", "64\n"},
            {2, "level", "2\n"},     {2, "type", "Unified\n"},
            {2, "size", "2048K\n"},  {2, "coherency_line_size", "64\n"},
            {3, "level", "3\n"},     {3, "type", "Unified\n"},
            {3, "size", "36864K\n"}, {3, "coherency_line_size", "64\n"},
        };
        dataspeedSysfsLayout_ layout = DS_LAYOUT_(x86);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysfs_(&got, dataspeedSysfsReadSample_, &layout);
        const dataspeedCacheInfo want = {.l1d_size = 48 * 1024,
                                         .l1i_size = 32 * 1024,
                                         .l2_size = 2048 * 1024,
                                         .l3_size = 36864 * 1024,
                                         .line_size = 64};
        err += dataspeedCheckCache_("x86 layout", &got, &want);
    }

    TEST("sysfs: instruction cache listed first, distinct line sizes") {
        static const dataspeedSysfsRow_ armish[] = {
            {0, "level", "1\n"},   {0, "type", "Instruction\n"},
            {0, "size", "192K\n"}, {0, "coherency_line_size", "128\n"},
            {1, "level", "1\n"},   {1, "type", "Data\n"},
            {1, "size", "128K\n"}, {1, "coherency_line_size", "64\n"},
            {2, "level", "2\n"},   {2, "type", "Unified\n"},
            {2, "size", "12M\n"},
        };
        dataspeedSysfsLayout_ layout = DS_LAYOUT_(armish);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysfs_(&got, dataspeedSysfsReadSample_, &layout);
        const dataspeedCacheInfo want = {.l1d_size = 128 * 1024,
                                         .l1i_size = 192 * 1024,
                                         .l2_size = 12 * 1024 * 1024,
                                         .line_size = 64};
        err += dataspeedCheckCache_("instruction-first layout", &got, &want);
    }

    TEST("sysfs: unified or untyped L1, skipped and malformed entries") {
        static const dataspeedSysfsRow_ unified[] = {
            {0, "level", "1\n"},
            {0, "size", "16K\n"}, /* no type file: unified */
            {1, "level", "x\n"},  /* unparsable level: skipped */
            {1, "type", "Data\n"},
            {1, "size", "999K\n"},
            {2, "level", "2\n"},
            {2, "type", "Instruction\n"}, /* not a data or unified L2 */
            {2, "size", "64K\n"},
            {3, "level", "2\n"},
            {3, "type", "Unified\n"},
            {3, "size", "256KB\n"}, /* malformed size: unknown */
            {3, "coherency_line_size", "32\n"},
            {5, "level", "3\n"}, /* after the gap at index 4: not read */
            {5, "size", "8M\n"},
        };
        dataspeedSysfsLayout_ layout = DS_LAYOUT_(unified);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysfs_(&got, dataspeedSysfsReadSample_, &layout);
        const dataspeedCacheInfo want = {
            .l1d_size = 16 * 1024, .l1i_size = 16 * 1024, .line_size = 32};
        err += dataspeedCheckCache_("unified layout", &got, &want);
    }

    TEST("sysfs: no cache entries at all") {
        static const dataspeedSysfsRow_ none[] = {{9, "level", "1\n"}};
        dataspeedSysfsLayout_ layout = DS_LAYOUT_(none);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysfs_(&got, dataspeedSysfsReadSample_, &layout);
        const dataspeedCacheInfo want = {.line_size =
                                             DATASPEED_DEFAULT_LINE_SIZE};
        err += dataspeedCheckCache_("empty layout", &got, &want);
    }

    TEST("sysctl: Apple hw.* layout") {
        static const dataspeedSysctlRow_ apple[] = {
            {"hw.l1dcachesize", 65536},
            {"hw.l1icachesize", 131072},
            {"hw.l2cachesize", 4194304},
            {"hw.cachelinesize", 128},
        };
        dataspeedSysctlLayout_ layout = DS_LAYOUT_(apple);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysctl_(&got, dataspeedSysctlReadSample_, &layout);
        const dataspeedCacheInfo want = {.l1d_size = 65536,
                                         .l1i_size = 131072,
                                         .l2_size = 4194304,
                                         .line_size = 128};
        err += dataspeedCheckCache_("apple sysctls", &got, &want);
    }

    TEST("sysctl: missing and nonsensical values") {
        static const dataspeedSysctlRow_ odd[] = {
            {"hw.l1dcachesize", 0},
            {"hw.l1icachesize", -1},
            {"hw.l3cachesize", 33554432},
            {"hw.cachelinesize", INT64_C(1) << 40},
        };
        dataspeedSysctlLayout_ layout = DS_LAYOUT_(odd);
        dataspeedCacheInfo got = {};
        dataspeedCacheFromSysctl_(&got, dataspeedSysctlReadSample_, &layout);
        const dataspeedCacheInfo want = {
            .l3_size = 33554432, .line_size = DATASPEED_DEFAULT_LINE_SIZE};
        err += dataspeedCheckCache_("odd sysctls", &got, &want);
    }

    TEST_FINAL_RESULT;
}
#endif
