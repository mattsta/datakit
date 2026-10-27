#ifndef CTEST_H
#define CTEST_H

/* Tests check with assert() as well as ERR, so a test build must keep
 * assertions: the build passes -UNDEBUG after any -DNDEBUG of the build
 * type, and a build where NDEBUG survives fails here instead of running
 * tests whose asserts compiled away */
#if defined(DATAKIT_TEST) && defined(NDEBUG)
#error "DATAKIT_TEST code needs assertions: NDEBUG must not be defined"
#endif

#include "dkRandom.h" /* dkLcg64Next: the tests' replayable stream */
#include <stdio.h>
#include <string.h> /* strchr */

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
