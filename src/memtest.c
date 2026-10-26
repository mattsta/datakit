/* memtest - In-process memory testing
 *
 * Copyright 2017 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "memtest.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_region.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||   \
    defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#if defined(__FreeBSD__)
#include <libprocstat.h>
#endif
#endif

#define IKNOWWHATIMDOING /* allow malloc usage */
#include "datakit.h"

/* Pattern constants */
#if (ULONG_MAX == 4294967295UL)
#define ULONG_ONEZERO 0xaaaaaaaaUL
#define ULONG_ZEROONE 0x55555555UL
#elif (ULONG_MAX == 18446744073709551615ULL)
#define ULONG_ONEZERO 0xaaaaaaaaaaaaaaaaUL
#define ULONG_ZEROONE 0x5555555555555555UL
#else
#error "ULONG_MAX value not supported."
#endif

/* ============================================================================
 * Timing Helpers
 * ============================================================================
 */
static uint64_t memtest_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Number Parsing
 * ============================================================================
 */
/* Parse all of 's' as an unsigned integer in 'base' (10 or 16): digits of
 * that base only, with no sign, "0x" prefix or blanks.  Writes *value and
 * returns true only when the value is at most 'max'; anything else
 * (including an empty string or a value past UINT64_MAX) returns false and
 * leaves *value alone. */
DK_FN_UNUSED static bool memtestParseU64_(const char *s, int base, uint64_t max,
                                          uint64_t *value) {
    if (*s == '\0') {
        return false;
    }

    /* Check the digits first: strtoull would accept blanks, a sign (and
     * negate "-1" into a huge value) and, in base 16, a "0x" prefix */
    for (const char *c = s; *c; c++) {
        const bool digit = *c >= '0' && *c <= '9';
        const bool hexLetter =
            (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
        if (!digit && !(base == 16 && hexLetter)) {
            return false;
        }
    }

    errno = 0;
    char *end = NULL;
    const uint64_t parsed = strtoull(s, &end, base);
    if (errno != 0 || *end != '\0' || parsed > max) {
        return false;
    }

    *value = parsed;
    return true;
}

/* Parse the address range that starts a BSD /proc/<pid>/map line:
 * "<start> <end> ...", each a hexadecimal address with an optional "0x"
 * prefix, separated by blanks.  Returns true only when both parse as
 * addresses and end >= start. */
DK_FN_UNUSED static bool
memtestParseBsdMapRange_(const char *line, uint64_t *start, uint64_t *end) {
    uint64_t bounds[2];
    const char *p = line;
    for (size_t i = 0; i < 2; i++) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }

        /* Copy the token: memtestParseU64_ parses a whole string */
        char token[24];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' && p[n] != '\n') {
            if (n + 1 >= sizeof(token)) {
                return false; /* longer than any 64-bit address */
            }

            token[n] = p[n];
            n++;
        }

        token[n] = '\0';
        if (!memtestParseU64_(token, 16, UINTPTR_MAX, &bounds[i])) {
            return false;
        }

        p += n;
    }

    if (bounds[1] < bounds[0]) {
        return false;
    }

    *start = bounds[0];
    *end = bounds[1];
    return true;
}

/* ============================================================================
 * Progress Reporting Context
 * ============================================================================
 */
typedef struct {
    memtestProgressFn callback;
    void *userdata;
    const char *phase;
    size_t total;
    size_t current;
    /* Terminal progress (for interactive mode) */
    struct winsize ws;
    size_t progress_printed;
    size_t progress_full;
    bool interactive;
} memtestContext;

static void ctx_init(memtestContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static void ctx_set_callback(memtestContext *ctx, memtestProgressFn fn,
                             void *userdata) {
    ctx->callback = fn;
    ctx->userdata = userdata;
}

/* The display draws below a 3-row header, so it needs 4+ rows and some
 * columns: fewer (a failed query, or a terminal reporting 0x0 or a sliver)
 * would make the progress width (cols * (rows - 3)) negative, wrapping to
 * an enormous count of characters to print.  Those get 80x20. */
static void memtestFitWindow_(struct winsize *ws, bool queried) {
    if (!queried || ws->ws_row < 4 || ws->ws_col == 0) {
        ws->ws_col = 80;
        ws->ws_row = 20;
    }
}

static void ctx_set_interactive(memtestContext *ctx) {
    ctx->interactive = true;
    const bool queried = ioctl(1, TIOCGWINSZ, &ctx->ws) != -1;
    memtestFitWindow_(&ctx->ws, queried);
}

static void ctx_start_phase(memtestContext *ctx, const char *phase,
                            size_t total) {
    ctx->phase = phase;
    ctx->total = total;
    ctx->current = 0;

    if (ctx->interactive) {
        printf("\x1b[H\x1b[2J"); /* Cursor home, clear screen */
        for (int j = 0; j < ctx->ws.ws_col * (ctx->ws.ws_row - 2); j++) {
            printf(".");
        }
        printf("\x1b[H\x1b[2K");
        printf("%s\n", phase);
        ctx->progress_printed = 0;
        ctx->progress_full = ctx->ws.ws_col * (ctx->ws.ws_row - 3);
        fflush(stdout);
    }

    if (ctx->callback) {
        ctx->callback(phase, 0.0, ctx->userdata);
    }
}

static void ctx_update(memtestContext *ctx, size_t current, char sym) {
    ctx->current = current;

    if (ctx->interactive && ctx->total > 0) {
        size_t chars = ((uint64_t)current * ctx->progress_full) / ctx->total;
        for (size_t j = ctx->progress_printed; j < chars; j++) {
            printf("%c", sym);
        }
        ctx->progress_printed = chars;
        fflush(stdout);
    }

    if (ctx->callback && ctx->total > 0) {
        double progress = (double)current / (double)ctx->total;
        ctx->callback(ctx->phase, progress, ctx->userdata);
    }
}

static void ctx_end_phase(memtestContext *ctx) {
    if (ctx->interactive) {
        printf("\x1b[H\x1b[2J");
        fflush(stdout);
    }
    if (ctx->callback) {
        ctx->callback(ctx->phase, 1.0, ctx->userdata);
    }
}

/* ============================================================================
 * Core Test Patterns
 * ============================================================================
 */

/* xorshift64* PRNG - no external dependencies */
#define XORSHIFT_NEXT(rseed, rout)                                             \
    do {                                                                       \
        (rseed) ^= (rseed) >> 12;                                              \
        (rseed) ^= (rseed) << 25;                                              \
        (rseed) ^= (rseed) >> 27;                                              \
        (rout) = (rseed) * UINT64_C(2685821657736338717);                      \
    } while (0)

/* Address test: each location stores its own address */
static int memtest_addressing_ctx(uint64_t *mem, size_t bytes,
                                  memtestContext *ctx) {
    size_t words = bytes / sizeof(uint64_t);
    uint64_t *p;

    if (ctx) {
        ctx_start_phase(ctx, "Addressing test", words * 2);
    }

    /* Fill phase */
    p = mem;
    for (size_t j = 0; j < words; j++) {
        *p = (uint64_t)(uintptr_t)p;
        p++;
        if (ctx && (j & 0xffff) == 0) {
            ctx_update(ctx, j, 'A');
        }
    }

    /* Verify phase */
    p = mem;
    for (size_t j = 0; j < words; j++) {
        if (*p != (uint64_t)(uintptr_t)p) {
            if (ctx) {
                ctx_end_phase(ctx);
            }
            return 1; /* Error found */
        }
        p++;
        if (ctx && (j & 0xffff) == 0) {
            ctx_update(ctx, words + j, 'A');
        }
    }

    if (ctx) {
        ctx_end_phase(ctx);
    }
    return 0;
}

int memtestAddressing(uint64_t *mem, size_t bytes) {
    return memtest_addressing_ctx(mem, bytes, NULL);
}

/* Random fill: page-strided access pattern with xorshift64* */
static void memtest_fill_random_ctx(uint64_t *mem, size_t bytes,
                                    memtestContext *ctx) {
    size_t step = 4096 / sizeof(uint64_t);
    size_t words = bytes / sizeof(uint64_t) / 2;
    size_t iwords = words / step;
    uint64_t rseed = UINT64_C(0xd13133de9afdb566);
    uint64_t rout = 0;

    assert((bytes & 4095) == 0);

    if (ctx) {
        ctx_start_phase(ctx, "Random fill", words);
    }

    for (size_t off = 0; off < step; off++) {
        uint64_t *l1 = mem + off;
        uint64_t *l2 = l1 + words;
        for (size_t w = 0; w < iwords; w++) {
            XORSHIFT_NEXT(rseed, rout);
            *l1 = *l2 = rout;
            l1 += step;
            l2 += step;
            if (ctx && (w & 0xffff) == 0) {
                ctx_update(ctx, w + iwords * off, 'R');
            }
        }
    }

    if (ctx) {
        ctx_end_phase(ctx);
    }
}

void memtestFillRandom(uint64_t *mem, size_t bytes) {
    memtest_fill_random_ctx(mem, bytes, NULL);
}

/* Pattern fill: alternating v1/v2 pattern */
static void memtest_fill_pattern_ctx(uint64_t *mem, size_t bytes, uint64_t v1,
                                     uint64_t v2, char sym,
                                     memtestContext *ctx) {
    size_t step = 4096 / sizeof(uint64_t);
    size_t words = bytes / sizeof(uint64_t) / 2;
    size_t iwords = words / step;

    assert((bytes & 4095) == 0);

    if (ctx) {
        const char *phase = (v1 == 0) ? "Solid fill" : "Checkerboard fill";
        ctx_start_phase(ctx, phase, words);
    }

    for (size_t off = 0; off < step; off++) {
        uint64_t *l1 = mem + off;
        uint64_t *l2 = l1 + words;
        uint64_t v = (off & 1) ? v2 : v1;
#ifdef MEMTEST_32BIT
        uint64_t pattern = ((uint64_t)v) | (((uint64_t)v) << 16);
#else
        uint64_t pattern = ((uint64_t)v) | (((uint64_t)v) << 16) |
                           (((uint64_t)v) << 32) | (((uint64_t)v) << 48);
#endif
        for (size_t w = 0; w < iwords; w++) {
            *l1 = *l2 = pattern;
            l1 += step;
            l2 += step;
            if (ctx && (w & 0xffff) == 0) {
                ctx_update(ctx, w + iwords * off, sym);
            }
        }
    }

    if (ctx) {
        ctx_end_phase(ctx);
    }
}

void memtestFillPattern(uint64_t *mem, size_t bytes, uint64_t v1, uint64_t v2) {
    memtest_fill_pattern_ctx(mem, bytes, v1, v2, 'P', NULL);
}

/* Compare: verify first half equals second half */
static int memtest_compare_ctx(uint64_t *mem, size_t bytes,
                               memtestContext *ctx) {
    size_t words = bytes / sizeof(uint64_t) / 2;
    uint64_t *l1 = mem;
    uint64_t *l2 = mem + words;
    int errors = 0;

    assert((bytes & 4095) == 0);

    if (ctx) {
        ctx_start_phase(ctx, "Compare", words);
    }

    for (size_t w = 0; w < words; w++) {
        if (*l1 != *l2) {
            errors++;
        }
        l1++;
        l2++;
        if (ctx && (w & 0xffff) == 0) {
            ctx_update(ctx, w, '=');
        }
    }

    if (ctx) {
        ctx_end_phase(ctx);
    }
    return errors;
}

int memtestCompare(uint64_t *mem, size_t bytes) {
    return memtest_compare_ctx(mem, bytes, NULL);
}

/* ============================================================================
 * Main Test Functions
 * ============================================================================
 */

/* Run a complete test pass */
static int memtest_run_pass(uint64_t *mem, size_t bytes, memtestContext *ctx) {
    int errors = 0;
    const int compare_times = 4;

    /* Address test */
    errors += memtest_addressing_ctx(mem, bytes, ctx);

    /* Random fill + compare */
    memtest_fill_random_ctx(mem, bytes, ctx);
    for (int i = 0; i < compare_times; i++) {
        errors += memtest_compare_ctx(mem, bytes, ctx);
    }

    /* Solid fill (0/0xFF) + compare */
    memtest_fill_pattern_ctx(mem, bytes, 0, (uint64_t)-1, 'S', ctx);
    for (int i = 0; i < compare_times; i++) {
        errors += memtest_compare_ctx(mem, bytes, ctx);
    }

    /* Checkerboard (0xAA/0x55) + compare */
    memtest_fill_pattern_ctx(mem, bytes, ULONG_ONEZERO, ULONG_ZEROONE, 'C',
                             ctx);
    for (int i = 0; i < compare_times; i++) {
        errors += memtest_compare_ctx(mem, bytes, ctx);
    }

    return errors;
}

/* Destructive test */
static size_t memtest_destructive(void *mem, size_t bytes, int passes,
                                  memtestContext *ctx, memtestResult *result) {
    uint64_t start = memtest_time_ns();
    size_t errors = 0;

    for (int pass = 0; pass < passes; pass++) {
        errors += memtest_run_pass(mem, bytes, ctx);
        if (result) {
            result->passes_complete = pass + 1;
        }
    }

    if (result) {
        result->bytes_tested = bytes;
        result->errors_found = errors;
        result->duration_s = (memtest_time_ns() - start) / 1e9;
    }

    return errors;
}

/* Non-destructive (preserving) test */
#define MEMTEST_BACKUP_WORDS (1024 * 1024 / sizeof(uint64_t))
#define MEMTEST_DECACHE_SIZE (1024 * 8)

static size_t memtest_preserving(void *mem, size_t bytes, int passes,
                                 memtestContext *ctx, memtestResult *result) {
    uint64_t backup[MEMTEST_BACKUP_WORDS];
    uint64_t *p = mem;
    /* The last decache window, only formed when the buffer holds one (a
     * pointer before the buffer's start would be undefined) */
    uint64_t *end =
        bytes >= MEMTEST_DECACHE_SIZE
            ? (uint64_t *)((char *)mem + bytes - MEMTEST_DECACHE_SIZE)
            : mem;
    size_t left = bytes;
    size_t errors = 0;
    uint64_t start = memtest_time_ns();

    if ((bytes & 4095) != 0 || bytes < 8192) {
        if (result) {
            result->bytes_tested = 0;
            result->errors_found = 0;
            result->passes_complete = 0;
            result->duration_s = 0;
        }
        return 0;
    }

    /* One phase for the whole run, its progress in hundredths of the
     * bytes done (the per-chunk tests run without a context) */
    if (ctx) {
        ctx_start_phase(ctx, "Preserving test", 100);
    }

    while (left) {
        /* Handle single final page */
        if (left == 4096) {
            left += 4096;
            p -= 4096 / sizeof(uint64_t);
        }

        size_t len = (left > sizeof(backup)) ? sizeof(backup) : left;
        if ((len / 4096) % 2) {
            len -= 4096;
        }

        memcpy(backup, p, len);

        for (int pass = 0; pass < passes; pass++) {
            errors += memtest_addressing_ctx(p, len, NULL);
            memtest_fill_random_ctx(p, len, NULL);

            /* Cache-defeating accesses */
            if (bytes >= MEMTEST_DECACHE_SIZE) {
                memtest_compare_ctx(mem, MEMTEST_DECACHE_SIZE, NULL);
                memtest_compare_ctx(end, MEMTEST_DECACHE_SIZE, NULL);
            }

            for (int i = 0; i < 4; i++) {
                errors += memtest_compare_ctx(p, len, NULL);
            }

            memtest_fill_pattern_ctx(p, len, 0, (uint64_t)-1, 'S', NULL);
            if (bytes >= MEMTEST_DECACHE_SIZE) {
                memtest_compare_ctx(mem, MEMTEST_DECACHE_SIZE, NULL);
                memtest_compare_ctx(end, MEMTEST_DECACHE_SIZE, NULL);
            }
            for (int i = 0; i < 4; i++) {
                errors += memtest_compare_ctx(p, len, NULL);
            }

            memtest_fill_pattern_ctx(p, len, ULONG_ONEZERO, ULONG_ZEROONE, 'C',
                                     NULL);
            if (bytes >= MEMTEST_DECACHE_SIZE) {
                memtest_compare_ctx(mem, MEMTEST_DECACHE_SIZE, NULL);
                memtest_compare_ctx(end, MEMTEST_DECACHE_SIZE, NULL);
            }
            for (int i = 0; i < 4; i++) {
                errors += memtest_compare_ctx(p, len, NULL);
            }
        }

        memcpy(p, backup, len);
        left -= len;
        p += len / sizeof(uint64_t);

        /* The end of the phase reports completion, once */
        if (ctx && left > 0) {
            double progress = (double)(bytes - left) / (double)bytes;
            ctx_update(ctx, (size_t)(progress * 100), '.');
        }
    }
    if (ctx) {
        ctx_end_phase(ctx);
    }

    if (result) {
        result->bytes_tested = bytes;
        result->errors_found = errors;
        result->passes_complete = passes;
        result->duration_s = (memtest_time_ns() - start) / 1e9;
    }

    return errors;
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

size_t memtest(void *mem, size_t bytes, int passes, bool preserving) {
    if (preserving) {
        return memtest_preserving(mem, bytes, passes, NULL, NULL);
    } else {
        return memtest_destructive(mem, bytes, passes, NULL, NULL);
    }
}

void memtestWithResult(void *mem, size_t bytes, int passes, bool preserving,
                       memtestResult *result) {
    if (preserving) {
        memtest_preserving(mem, bytes, passes, NULL, result);
    } else {
        memtest_destructive(mem, bytes, passes, NULL, result);
    }
}

void memtestWithProgress(void *mem, size_t bytes, int passes, bool preserving,
                         memtestProgressFn progress_fn, void *userdata,
                         memtestResult *result) {
    memtestContext ctx;
    ctx_init(&ctx);
    ctx_set_callback(&ctx, progress_fn, userdata);

    if (preserving) {
        memtest_preserving(mem, bytes, passes, &ctx, result);
    } else {
        memtest_destructive(mem, bytes, passes, &ctx, result);
    }
}

size_t memtestAllocAndTest(size_t megabytes, int passes,
                           memtestResult *result) {
    size_t bytes = megabytes * 1024 * 1024;
    void *mem = malloc(bytes);

    if (!mem) {
        if (result) {
            result->bytes_tested = 0;
            result->errors_found = 0;
            result->passes_complete = 0;
            result->duration_s = 0;
        }
        return (size_t)-1; /* Allocation failure */
    }

    size_t errors = memtest_destructive(mem, bytes, passes, NULL, result);
    free(mem);
    return errors;
}

void memtestInteractive(size_t megabytes, int passes, memtestResult *result) {
    size_t bytes = megabytes * 1024 * 1024;
    void *mem = malloc(bytes);

    if (!mem) {
        fprintf(stderr, "Unable to allocate %zu MB: %s\n", megabytes,
                strerror(errno));
        if (result) {
            memset(result, 0, sizeof(*result));
        }
        return;
    }

    memtestContext ctx;
    ctx_init(&ctx);
    ctx_set_interactive(&ctx);

    memtest_destructive(mem, bytes, passes, &ctx, result);
    free(mem);

    /* Clear screen and show result */
    printf("\x1b[H\x1b[2J");
    if (result && result->errors_found == 0) {
        printf("Memory test PASSED\n");
        printf("Tested: %zu MB, %zu passes, %.1f seconds\n", megabytes,
               result->passes_complete, result->duration_s);
    } else if (result) {
        printf("Memory test FAILED: %zu errors detected\n",
               result->errors_found);
    }
}

/* ============================================================================
 * Platform-Specific Process Memory Test
 * ============================================================================
 */

#if defined(__linux__)
#define MEMTEST_MAX_REGIONS 128

size_t memtestProcessMemory(int passes) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return 0;
    }

    char line[1024];
    size_t start_vect[MEMTEST_MAX_REGIONS];
    size_t size_vect[MEMTEST_MAX_REGIONS];
    int regions = 0;

    while (fgets(line, sizeof(line), fp) && regions < MEMTEST_MAX_REGIONS) {
        char *start = line;
        char *p = strchr(line, '-');
        if (!p) {
            continue;
        }
        *p++ = '\0';
        char *end = p;
        p = strchr(p, ' ');
        if (!p) {
            continue;
        }
        *p++ = '\0';

        /* Skip special regions */
        if (strstr(p, "stack") || strstr(p, "vdso") || strstr(p, "vsyscall")) {
            continue;
        }
        if (!strstr(p, "00:00") || !strstr(p, "rw")) {
            continue;
        }

        /* Skip a line whose range does not parse, or ends before it starts */
        uint64_t startAddr = 0;
        uint64_t endAddr = 0;
        if (!memtestParseU64_(start, 16, UINTPTR_MAX, &startAddr) ||
            !memtestParseU64_(end, 16, UINTPTR_MAX, &endAddr) ||
            endAddr < startAddr) {
            continue;
        }

        start_vect[regions] = (size_t)startAddr;
        size_vect[regions] = (size_t)(endAddr - startAddr);
        regions++;
    }

    fclose(fp);

    size_t errors = 0;
    for (int i = 0; i < regions; i++) {
        errors += memtest_preserving((void *)start_vect[i], size_vect[i],
                                     passes, NULL, NULL);
    }

    return errors;
}

#elif defined(__APPLE__)

/* macOS: Process memory testing via Mach VM is unreliable due to guard pages
 * and other protected regions that can cause crashes. Return unsupported. */
size_t memtestProcessMemory(int passes) {
    (void)passes;
    return 0; /* Not reliably supported on macOS */
}

#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||   \
    defined(__DragonFly__)

/* BSD: Try /proc if mounted, otherwise not supported */
size_t memtestProcessMemory(int passes) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/map", (int)getpid());

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* procfs not mounted - can't enumerate memory regions */
        return 0;
    }

    char line[1024];
    size_t errors = 0;

    /* BSD /proc/pid/map format: start end resident private_resident ... */
    while (fgets(line, sizeof(line), fp)) {
        /* Skip a line whose range does not parse, or ends before it starts */
        uint64_t startAddr = 0;
        uint64_t endAddr = 0;
        if (!memtestParseBsdMapRange_(line, &startAddr, &endAddr)) {
            continue;
        }

        const size_t start_addr = (size_t)startAddr;
        const size_t end_addr = (size_t)endAddr;

        /* Check for rw permissions in the line */
        if (!strstr(line, "rw")) {
            continue;
        }

        size_t size = end_addr - start_addr;
        if (size >= 8192) {
            errors += memtest_preserving((void *)start_addr, size, passes, NULL,
                                         NULL);
        }
    }

    fclose(fp);
    return errors;
}

#else

size_t memtestProcessMemory(int passes) {
    (void)passes;
    return 0; /* Not supported on this platform */
}

#endif

/* ============================================================================
 * Test Suite
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "ctest.h"

static void memtest_print_usage(void) {
    printf("Usage: datakit-test test memtest [options]\n\n");
    printf("Options:\n");
    printf("  (no args)     Run quick unit tests (default)\n");
    printf("  <MB>          Test specified megabytes of memory\n");
    printf("  <MB> <passes> Test with specified number of passes\n");
    printf("  --process     Test process memory (Linux only)\n");
    printf("\nExamples:\n");
    printf("  memtest              Quick unit tests\n");
    printf("  memtest 1024         Test 1 GB with 1 pass\n");
    printf("  memtest 4096 3       Test 4 GB with 3 passes\n");
    printf("\nFor thorough testing, run several minutes per GB.\n");
}

/* Records progress calls: a phase opens at 0.0, rises without going back
 * and stays within [0, 1], and closes at 1.0 */
typedef struct mtProgressLog {
    const char *phase; /* the open phase; NULL between phases */
    double last;
    size_t phases;
    bool open;
    bool bad;
} mtProgressLog;

static void mtProgressNote_(const char *phase, double progress, void *ud) {
    mtProgressLog *log = ud;
    if (!log->open) {
        log->bad = log->bad || !phase || progress != 0.0;
        log->phase = phase;
        log->last = 0.0;
        log->open = true;
        log->phases++;
        return;
    }
    log->bad = log->bad || phase != log->phase || progress < log->last ||
               progress > 1.0;
    log->last = progress;
    if (progress == 1.0) {
        log->open = false;
    }
}

static int memtest_run_unit_tests(void) {
    int err = 0;

    /* Test addressing on small buffer */
    TEST("addressing - 64KB") {
        size_t bytes = 64 * 1024;
        uint64_t *mem = aligned_alloc(4096, bytes);
        assert(mem);
        int errors = memtestAddressing(mem, bytes);
        if (errors != 0) {
            ERRR("Addressing test found errors on good memory");
        }
        free(mem);
    }

    /* Test random fill + compare */
    TEST("random fill - 64KB") {
        size_t bytes = 64 * 1024;
        uint64_t *mem = aligned_alloc(4096, bytes);
        assert(mem);
        memtestFillRandom(mem, bytes);
        int errors = memtestCompare(mem, bytes);
        if (errors != 0) {
            ERRR("Random fill does not produce matching halves");
        }
        free(mem);
    }

    /* Test pattern fill + compare */
    TEST("pattern fill - 64KB") {
        size_t bytes = 64 * 1024;
        uint64_t *mem = aligned_alloc(4096, bytes);
        assert(mem);

        memtestFillPattern(mem, bytes, 0, (uint64_t)-1);
        int errors = memtestCompare(mem, bytes);
        if (errors != 0) {
            ERRR("Solid fill does not produce matching halves");
        }

        memtestFillPattern(mem, bytes, ULONG_ONEZERO, ULONG_ZEROONE);
        errors = memtestCompare(mem, bytes);
        if (errors != 0) {
            ERRR("Checkerboard fill does not produce matching halves");
        }

        free(mem);
    }

    /* Test full pass on 1MB */
    TEST("full test - 1MB") {
        size_t bytes = 1024 * 1024;
        void *mem = aligned_alloc(4096, bytes);
        assert(mem);

        memtestResult result;
        memtestWithResult(mem, bytes, 1, false, &result);

        if (result.errors_found != 0) {
            ERR("Found %zu errors on 1MB test", result.errors_found);
        }
        if (result.passes_complete != 1) {
            ERRR("Did not complete 1 pass");
        }
        if (result.bytes_tested != bytes) {
            ERRR("Did not test correct size");
        }
        if (result.duration_s <= 0) {
            ERRR("Duration not recorded");
        }

        free(mem);
    }

    /* Test preserving mode */
    TEST("interactive display sizes stay positive") {
        /* A failed query or a terminal under 4 rows or with no columns
         * gets 80x20; a usable one is kept; either way the progress width
         * cols * (rows - 3) is positive */
        static const struct {
            unsigned short rows, cols;
            bool queried;
            unsigned short wantRows, wantCols;
        } cases[] = {
            {0, 0, true, 20, 80},     {1, 120, true, 20, 80},
            {3, 120, true, 20, 80},   {4, 0, true, 20, 80},
            {4, 50, true, 4, 50},     {50, 200, true, 50, 200},
            {50, 200, false, 20, 80},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            struct winsize ws = {.ws_row = cases[i].rows,
                                 .ws_col = cases[i].cols};
            memtestFitWindow_(&ws, cases[i].queried);
            if (ws.ws_row != cases[i].wantRows ||
                ws.ws_col != cases[i].wantCols ||
                ws.ws_col * (ws.ws_row - 3) <= 0) {
                ERR("window %zu fit to %ux%u", i, ws.ws_col, ws.ws_row);
            }
        }
    }

    TEST("progress reports every phase from 0 to 1, in order") {
        /* Both modes, two passes over 64 KiB: every phase reports 0.0
         * first, then non-decreasing progress within [0, 1], then 1.0;
         * no errors, both passes done; memtest() agrees; preserving mode
         * leaves the contents as they were */
        size_t bytes = 64 * 1024;
        uint64_t *mem = aligned_alloc(4096, bytes);
        for (int preserving = 0; preserving <= 1; preserving++) {
            for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
                mem[i] = i * 0x9e3779b97f4a7c15ULL;
            }
            mtProgressLog log = {};
            memtestResult result = {};
            memtestWithProgress(mem, bytes, 2, preserving, mtProgressNote_,
                                &log, &result);
            bool kept = true;
            for (size_t i = 0; preserving && i < bytes / sizeof(uint64_t);
                 i++) {
                kept = kept && mem[i] == i * 0x9e3779b97f4a7c15ULL;
            }
            if (log.bad || log.phases == 0 || log.open ||
                result.errors_found != 0 || result.passes_complete != 2 ||
                !kept || memtest(mem, bytes, 1, preserving) != 0) {
                ERR("preserving %d: %zu phases, bad %d, open %d, errors "
                    "%zu, passes %zu, kept %d",
                    preserving, log.phases, log.bad, log.open,
                    result.errors_found, result.passes_complete, kept);
            }
        }
        free(mem);
    }

    TEST("preserving test - 64KB") {
        size_t bytes = 64 * 1024;
        uint64_t *mem = aligned_alloc(4096, bytes);
        assert(mem);

        /* Fill with known pattern */
        for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
            mem[i] = 0xDEADBEEFCAFEBABEULL;
        }

        memtestResult result;
        memtestWithResult(mem, bytes, 1, true, &result);

        if (result.errors_found != 0) {
            ERR("Found %zu errors in preserving mode", result.errors_found);
        }

        /* Verify pattern preserved */
        bool preserved = true;
        for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
            if (mem[i] != 0xDEADBEEFCAFEBABEULL) {
                preserved = false;
                break;
            }
        }
        if (!preserved) {
            ERRR("Original data not preserved after test");
        }

        free(mem);
    }

    TEST("number parsing accepts whole numbers only") {
        static const struct {
            const char *s;
            int base;
            uint64_t max;
            bool ok;
            uint64_t want;
        } cases[] = {
            {"1", 10, 1000, true, 1},
            {"1000", 10, 1000, true, 1000},
            {"0", 10, 1000, true, 0},
            {"007", 10, 1000, true, 7},
            {"1001", 10, 1000, false, 0},
            {"18446744073709551615", 10, UINT64_MAX, true, UINT64_MAX},
            {"18446744073709551616", 10, UINT64_MAX, false, 0},
            {"99999999999999999999999", 10, UINT64_MAX, false, 0},
            {"", 10, 1000, false, 0},
            {"-1", 10, UINT64_MAX, false, 0},
            {"+1", 10, 1000, false, 0},
            {" 1", 10, 1000, false, 0},
            {"1 ", 10, 1000, false, 0},
            {"12x", 10, 1000, false, 0},
            {"1a", 10, 1000, false, 0},
            {"7f12abcd0000", 16, UINT64_MAX, true, 0x7f12abcd0000ULL},
            {"FFFFFFFFFFFFFFFF", 16, UINT64_MAX, true, UINT64_MAX},
            {"10000000000000000", 16, UINT64_MAX, false, 0},
            {"0x10", 16, UINT64_MAX, false, 0},
            {"-1", 16, UINT64_MAX, false, 0},
            {"ag", 16, UINT64_MAX, false, 0},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uint64_t got = 12345;
            const bool ok =
                memtestParseU64_(cases[i].s, cases[i].base, cases[i].max, &got);
            if (ok != cases[i].ok) {
                ERR("parse '%s' base %d: returned %d, want %d", cases[i].s,
                    cases[i].base, ok, cases[i].ok);
            } else if (ok && got != cases[i].want) {
                ERR("parse '%s' base %d: got %" PRIu64 ", want %" PRIu64,
                    cases[i].s, cases[i].base, got, cases[i].want);
            } else if (!ok && got != 12345) {
                ERR("parse '%s' base %d: failed but wrote %" PRIu64, cases[i].s,
                    cases[i].base, got);
            }
        }
    }

    TEST("BSD /proc map lines parse to checked address ranges") {
        static const struct {
            const char *line;
            bool ok;
            uint64_t start;
            uint64_t end;
        } cases[] = {
            {"0x400000 0x401000 1 0 0xfffff80 rw- 1 0 0x1000 COW NC vnode\n",
             true, 0x400000, 0x401000},
            {"0x800a00000 0x800a21000 33 0 0x0 rwx 2 0 0x1000\n", true,
             0x800a00000ULL, 0x800a21000ULL},
            {"400000 401000 rw-\n", true, 0x400000, 0x401000},
            {"  0X10\t0X20 rw-\n", true, 0x10, 0x20},
            {"0xffffffffffffffff 0xffffffffffffffff rw-\n", true, UINT64_MAX,
             UINT64_MAX},
            {"0x10000000000000000 0x10000000000000001 rw-\n", false, 0, 0},
            {"0x2000 0x1000 rw-\n", false, 0, 0}, /* ends before start */
            {"0x2000\n", false, 0, 0},
            {"0x2000 rw-\n", false, 0, 0},
            {"-0x10 0x20 rw-\n", false, 0, 0},
            {"0x 0x20 rw-\n", false, 0, 0},
            {"0x10g 0x20 rw-\n", false, 0, 0},
            {"", false, 0, 0},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uint64_t start = 7;
            uint64_t end = 7;
            const bool ok =
                memtestParseBsdMapRange_(cases[i].line, &start, &end);
            if (ok != cases[i].ok ||
                (ok && (start != cases[i].start || end != cases[i].end)) ||
                (!ok && (start != 7 || end != 7))) {
                ERR("map line %zu: ok=%d start=%" PRIx64 " end=%" PRIx64, i, ok,
                    start, end);
            }
        }
    }

    TEST("command line rejects malformed sizes and passes") {
        static const char *bad[][2] = {
            {"abc", NULL}, {"12x", NULL},     {"-1", NULL},
            {"0", NULL},   {"1048577", NULL}, {"99999999999999999999", NULL},
            {"1", "0"},    {"1", "1001"},     {"1", "2x"},
            {"1", "-3"},
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char *argv[3] = {"memtest", (char *)bad[i][0], (char *)bad[i][1]};
            const int argc = bad[i][1] ? 3 : 2;
            if (memtestTest(argc, argv) != 1) {
                ERR("memtest '%s' '%s' should be rejected", bad[i][0],
                    bad[i][1] ? bad[i][1] : "");
            }
        }
    }

    /* Test allocate and test */
    TEST("alloc and test - 1MB") {
        memtestResult result;
        size_t errors = memtestAllocAndTest(1, 1, &result);
        if (errors != 0) {
            ERR("Found %zu errors on allocated memory", errors);
        }
        if (result.bytes_tested != 1024 * 1024) {
            ERRR("Did not test 1MB");
        }
    }

    TEST_FINAL_RESULT;
}

int memtestTest(int argc, char *argv[]) {
    /* argc includes test name at argv[0], real args start at argv[1] */
    int nargs = argc - 1;
    char **args = argv + 1;

    /* No args: run unit tests */
    if (nargs == 0) {
        return memtest_run_unit_tests();
    }

    /* Help */
    if (strcmp(args[0], "-h") == 0 || strcmp(args[0], "--help") == 0) {
        memtest_print_usage();
        return 0;
    }

    /* Process memory test (Linux only) */
    if (strcmp(args[0], "--process") == 0) {
#if defined(__linux__)
        printf("memtest: process memory... ");
        fflush(stdout);
        size_t errors = memtestProcessMemory(1);
        if (errors == 0) {
            printf("PASSED\n");
            return 0;
        } else {
            printf("%zu ERRORS\n", errors);
            return 1;
        }
#else
        printf("memtest: --process is only supported on Linux\n");
        return 1;
#endif
    }

    /* Parse MB and optional passes: whole decimal numbers only, so "12x",
     * "-1" or an out-of-range value is an error, never a silent 0 or wrap */
    uint64_t megabytesArg = 0;
    if (!memtestParseU64_(args[0], 10, 1024 * 1024, &megabytesArg) ||
        megabytesArg == 0) {
        fprintf(stderr,
                "Error: Invalid size '%s' (must be a whole number of MB, "
                "1-%zu)\n",
                args[0], (size_t)(1024 * 1024));
        memtest_print_usage();
        return 1;
    }
    const size_t megabytes = (size_t)megabytesArg;
    int passes = 1;

    if (nargs >= 2) {
        uint64_t passesArg = 0;
        if (!memtestParseU64_(args[1], 10, 1000, &passesArg) ||
            passesArg == 0) {
            fprintf(stderr,
                    "Error: Invalid passes '%s' (must be a whole number, "
                    "1-1000)\n",
                    args[1]);
            return 1;
        }
        passes = (int)passesArg;
    }

    /* Run the full memory test with live progress */
    printf("memtest: %zu MB, %d pass%s\n", megabytes, passes,
           passes == 1 ? "" : "es");

    size_t bytes = megabytes * 1024 * 1024;
    void *mem = malloc(bytes);
    if (!mem) {
        printf("  FAILED: cannot allocate %zu MB\n", megabytes);
        return 1;
    }

    uint64_t start = memtest_time_ns();
    size_t total_errors = 0;

    for (int pass = 0; pass < passes; pass++) {
        printf("  pass %d/%d: ", pass + 1, passes);
        fflush(stdout);

        /* Address test */
        printf("addr");
        fflush(stdout);
        int errors = memtestAddressing(mem, bytes);
        total_errors += errors;

        /* Random fill + compare */
        printf(" random");
        fflush(stdout);
        memtestFillRandom(mem, bytes);
        for (int i = 0; i < 4; i++) {
            total_errors += memtestCompare(mem, bytes);
        }

        /* Solid fill + compare */
        printf(" solid");
        fflush(stdout);
        memtestFillPattern(mem, bytes, 0, (uint64_t)-1);
        for (int i = 0; i < 4; i++) {
            total_errors += memtestCompare(mem, bytes);
        }

        /* Checkerboard + compare */
        printf(" checker");
        fflush(stdout);
        memtestFillPattern(mem, bytes, ULONG_ONEZERO, ULONG_ZEROONE);
        for (int i = 0; i < 4; i++) {
            total_errors += memtestCompare(mem, bytes);
        }

        printf(" OK\n");
    }

    free(mem);

    double duration = (memtest_time_ns() - start) / 1e9;
    double throughput = (bytes * passes) / (duration * 1024 * 1024);

    printf("  ---\n");
    printf("  %zu MB × %d passes in %.1fs (%.0f MB/s)\n", megabytes, passes,
           duration, throughput);

    if (total_errors == 0) {
        printf("  PASSED\n");
        return 0;
    } else {
        printf("  FAILED: %zu errors\n", total_errors);
        return 1;
    }
}
#endif
