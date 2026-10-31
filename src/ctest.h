#ifndef CTEST_H
#define CTEST_H

/* Tests check with assert() as well as ERR, so a test build must keep
 * assertions: the build passes -UNDEBUG after any -DNDEBUG of the build
 * type, and a build where NDEBUG survives fails here instead of running
 * tests whose asserts compiled away */
#if defined(DATAKIT_TEST) && defined(NDEBUG)
#error "DATAKIT_TEST code needs assertions: NDEBUG must not be defined"
#endif

#include "datakit.h"  /* datakitConfig: the heap meter's allocator */
#include "dkEndian.h" /* dkStoreLe64: replayed bytes in one order */
#include "dkRandom.h" /* dkSplitmix64Next: the tests' replayable stream */
#include <stdio.h>
#include <string.h> /* strchr */
#include <unistd.h> /* _exit */

/* Leave a forked test child.  _exit skips the exit handlers, where a
 * coverage build writes its profile, so the child writes it first (one
 * file per process) and the calls it made are counted */
#ifdef DATAKIT_COVERAGE
int __llvm_profile_write_file(void);
#endif
[[noreturn]] [[maybe_unused]] static void ctestChildExit(int status) {
#ifdef DATAKIT_COVERAGE
    (void)__llvm_profile_write_file();
#endif
    _exit(status);
}

/* The heap meter: the net heap bytes allocated through datakit's
 * allocator between ctestHeapMeterStart and ctestHeapMeterStop, each
 * block counted at its usable size (zusablesize), so allocator slack,
 * pointer tables and headers count as they cost.  Frees subtract, blocks
 * from before the start included, so the result is the change in live
 * heap.  One meter runs at a time, on the thread doing the work. */
[[maybe_unused]] static datakitConfig ctestHeapSaved_;
[[maybe_unused]] static int64_t ctestHeapLive_;
[[maybe_unused]] static bool ctestHeapRunning_;

[[maybe_unused]] static int64_t ctestHeapSize_(const void *ptr) {
    return ptr ? (int64_t)ctestHeapSaved_.localUsableSize(ptr) : 0;
}

[[maybe_unused]] static void *ctestHeapCalloc_(size_t count, size_t sz) {
    void *ptr = ctestHeapSaved_.localCalloc(count, sz);
    ctestHeapLive_ += ctestHeapSize_(ptr);
    return ptr;
}

[[maybe_unused]] static void *ctestHeapMalloc_(size_t sz) {
    void *ptr = ctestHeapSaved_.localMalloc(sz);
    ctestHeapLive_ += ctestHeapSize_(ptr);
    return ptr;
}

/* A realloc through 'resize': a failure (NULL for a nonzero size) leaves
 * the old block live; a zero-size NULL result freed it */
[[maybe_unused]] static void *ctestHeapResize_(void *(*resize)(void *, size_t),
                                               void *ptr, size_t sz) {
    const int64_t was = ctestHeapSize_(ptr);
    void *out = resize(ptr, sz);
    if (out || sz == 0) {
        ctestHeapLive_ += ctestHeapSize_(out) - was;
    }

    return out;
}

[[maybe_unused]] static void *ctestHeapRealloc_(void *ptr, size_t sz) {
    return ctestHeapResize_(ctestHeapSaved_.localRealloc, ptr, sz);
}

[[maybe_unused]] static void *ctestHeapReallocSlate_(void *ptr, size_t sz) {
    return ctestHeapResize_(ctestHeapSaved_.localReallocSlate, ptr, sz);
}

[[maybe_unused]] static int ctestHeapMemalign_(void **memptr, size_t alignment,
                                               size_t size) {
    const int result = ctestHeapSaved_.localMemalign(memptr, alignment, size);
    if (result == 0) {
        ctestHeapLive_ += ctestHeapSize_(*memptr);
    }

    return result;
}

[[maybe_unused]] static void ctestHeapFree_(void *ptr) {
    ctestHeapLive_ -= ctestHeapSize_(ptr);
    ctestHeapSaved_.localFree(ptr);
}

/* Start metering; false (nothing changes) when a meter already runs or
 * the allocator cannot report usable sizes */
[[maybe_unused]] static bool ctestHeapMeterStart(void) {
    if (ctestHeapRunning_ || !datakitConfigMemory__.localUsableSize) {
        return false;
    }

    ctestHeapSaved_ = datakitConfigMemory__;
    ctestHeapLive_ = 0;
    ctestHeapRunning_ = true;
    datakitConfig metered = ctestHeapSaved_;
    metered.localCalloc = ctestHeapCalloc_;
    metered.localMalloc = ctestHeapMalloc_;
    metered.localRealloc = ctestHeapRealloc_;
    metered.localReallocSlate = ctestHeapReallocSlate_;
    metered.localMemalign = ctestHeapMemalign_;
    metered.localFree = ctestHeapFree_;
    datakitConfigSet(&metered);
    return true;
}

/* Stop metering, restoring the allocator: the net heap bytes since the
 * start (0 when no meter ran) */
[[maybe_unused]] static int64_t ctestHeapMeterStop(void) {
    if (!ctestHeapRunning_) {
        return 0;
    }

    datakitConfigSet(&ctestHeapSaved_);
    ctestHeapRunning_ = false;
    return ctestHeapLive_;
}

/* The 'len' bytes at 'buf' filled with dkRandom64 draws: up to 64 KiB of
 * them, then copies of those to the end, so a buffer of gigabytes costs
 * copies rather than draws.  Different on every run, like the draws. */
[[maybe_unused]] static void ctestRandomBytes(void *buf, size_t len) {
    uint8_t *b = buf;
    constexpr size_t fresh = 64 * 1024;
    size_t have = 0;
    while (have < len && have < fresh) {
        const uint64_t draw = dkRandom64();
        const size_t n = len - have < sizeof(draw) ? len - have : sizeof(draw);
        memcpy(b + have, &draw, n);
        have += n;
    }

    /* Each copy reads only bytes already written: [0, n) with n <= have */
    while (have < len) {
        const size_t n = len - have < have ? len - have : have;
        memcpy(b + have, b, n);
        have += n;
    }
}

/* The 'len' bytes at 'buf' filled from the replayable splitmix64 stream
 * at '*state', eight little-endian bytes per draw (a partial last draw
 * gives its low bytes), so the same seed gives the same bytes on every
 * platform.  A test drawing in turn keeps one state; a fill of its own
 * is ctestReplaySeeded. */
[[maybe_unused]] static void ctestReplayBytes(void *buf, size_t len,
                                              uint64_t *state) {
    uint8_t *b = buf;
    while (len >= sizeof(uint64_t)) {
        dkStoreLe64(b, dkSplitmix64Next(state));
        b += sizeof(uint64_t);
        len -= sizeof(uint64_t);
    }

    if (len) {
        uint8_t last[sizeof(uint64_t)];
        dkStoreLe64(last, dkSplitmix64Next(state));
        memcpy(b, last, len);
    }
}

/* ctestReplayBytes from a stream of its own, seeded with 'seed' */
[[maybe_unused]] static void ctestReplaySeeded(void *buf, size_t len,
                                               uint64_t seed) {
    ctestReplayBytes(buf, len, &seed);
}

/* Fill 8 KiB of the stack below the caller with 0xFF, so a decoder that
 * reads a box (or any temporary) before setting it up sees big = 1 and an
 * all-ones pointer instead of lucky zeros.  AddressSanitizer is off here
 * so redzones do not frame the array and leave the top of the next frame
 * unpoisoned. */
[[maybe_unused]] [[gnu::noinline, gnu::no_sanitize("address")]] static void
ctestPoisonStack(void) {
    volatile uint8_t junk[8192];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xFF;
    }
}

/* Generate new string concatenating integer i against string 'prefix'.
 * Marked unused: a TU may enable the generator but reference only some. */
#define CTEST_INCLUDE_GEN(named)                                               \
    [[maybe_unused]] static char *gen##named(const char *prefix, int i) {      \
        static char result[64] = {};                                           \
        memset(result, 0, sizeof(result));                                     \
        snprintf(result, sizeof(result), "%s%d", prefix, i);                   \
        return result;                                                         \
    }

#ifdef CTEST_INCLUDE_KVGEN
#define CTEST_INCLUDE_KEYGEN
#define CTEST_INCLUDE_VALGEN
#endif

#ifdef CTEST_INCLUDE_KEYGEN
CTEST_INCLUDE_GEN(key)
#endif

#ifdef CTEST_INCLUDE_VALGEN
CTEST_INCLUDE_GEN(val)
#endif

/* Only print the filename as debug prefix, not entire file path.
 * Just: detect the last '/' in file path, then increment one after it.
 *       if no '/' detected, use entire filename. */
#define currentFilename()                                                      \
    do {                                                                       \
        char *filenameOnly = strrchr(__FILE__, '/');                           \
        char *filePos = filenameOnly ? ++filenameOnly : __FILE__;              \
        printf("%s:%s:%d\t", filePos, __func__, __LINE__);                     \
    } while (0)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        fflush(stdout);                                                        \
        err++;                                                                 \
    } while (0)

/* A test failure: the message is a printf format, with or without
 * arguments */
#define ERR(x, ...)                                                            \
    do {                                                                       \
        currentFilename();                                                     \
        printf("ERROR! " x "\n" __VA_OPT__(, ) __VA_ARGS__);                   \
        fflush(stdout);                                                        \
        err++;                                                                 \
    } while (0)

/* 'out' gets the 'n' bytes a hex test vector spells; a vector that is not
 * exactly 2n hex digits is reported and adds one to 'failures' (the
 * test's counter: err or failures).  Needs str.h for StrHexToBytes. */
#define TEST_HEX(failures, hex, out, n)                                        \
    do {                                                                       \
        const char *hex_ = (hex);                                              \
        if (!StrHexToBytes(hex_, strlen(hex_), (out), (n))) {                  \
            currentFilename();                                                 \
            printf("ERROR! hex vector is not %zu bytes: %s\n", (size_t)(n),    \
                   hex_);                                                      \
            fflush(stdout);                                                    \
            (failures)++;                                                      \
        }                                                                      \
    } while (0)

/* A line of `label` then the `n` bytes at `p` in hex */
[[maybe_unused]] [[gnu::cold]] static void
ctestPrintHex_(const char *label, const void *p, size_t n) {
    printf("%s", label);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", ((const unsigned char *)p)[i]);
    }
    printf("\n");
}

/* Ends a TEST_BYTES report of `n` unequal bytes: where they first
 * differ, then both sides in hex: all of them up to 64 bytes, else the
 * 64 from the 16-byte line holding the first difference */
[[maybe_unused]] [[gnu::cold, gnu::noinline]] static void
ctestReportBytes_(const void *got, const void *want, size_t n) {
    const unsigned char *g = got;
    const unsigned char *w = want;
    size_t first = 0;
    while (first < n && g[first] == w[first]) {
        first++;
    }

    constexpr size_t window = 64;
    size_t start = 0;
    size_t shown = n;
    if (n > window) {
        start = first / 16 * 16;
        shown = n - start < window ? n - start : window;
    }

    printf(": %zu bytes, first difference at offset %zu\n", n, first);
    if (shown != n) {
        printf("  bytes [%zu, %zu):\n", start, start + shown);
    }

    ctestPrintHex_("  got:  ", g + start, shown);
    ctestPrintHex_("  want: ", w + start, shown);
}

/* The `n` bytes at `got` must equal those at `want`; a mismatch is
 * reported, described by the printf format and arguments that follow,
 * with the first differing offset and both sides in hex, and adds one
 * to `failures` (the test's counter: err or failures) */
#define TEST_BYTES(failures, got, want, n, ...)                                \
    do {                                                                       \
        const void *got_ = (got);                                              \
        const void *want_ = (want);                                            \
        const size_t n_ = (n);                                                 \
        if (memcmp(got_, want_, n_) != 0) {                                    \
            currentFilename();                                                 \
            printf("ERROR! ");                                                 \
            printf(__VA_ARGS__);                                               \
            ctestReportBytes_(got_, want_, n_);                                \
            fflush(stdout);                                                    \
            (failures)++;                                                      \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    printf("test — %s\n", name);                                               \
    fflush(stdout);
#define TEST_DESC(name, ...)                                                   \
    printf("test — " name "\n", __VA_ARGS__);                                  \
    fflush(stdout);

#define TEST_FINAL_RESULT                                                      \
    do {                                                                       \
        if (err) {                                                             \
            ERR("Sorry, not all tests passed!  In fact, %d tests failed.",     \
                err);                                                          \
        } else {                                                               \
            printf("ALL TESTS PASSED!\n");                                     \
        }                                                                      \
        return err;                                                            \
    } while (0)

#endif /* CTEST_H */
