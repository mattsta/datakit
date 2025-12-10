#include "fibbuf.h"
#include "jebuf.h"
#include <assert.h>

/* fibbuf: get a next buffer size based on fibonacci (sub-exponential) growth */

static uint16_t fibbufNextBuffer16(const uint16_t currentBufSize);
static uint32_t fibbufNextBuffer32(const uint32_t currentBufSize);
static uint64_t fibbufNextBuffer64(const uint64_t currentBufSize);

size_t fibbufNextSizeAllocation(const size_t currentBufSize) {
    return jebufSizeAllocation(fibbufNextSizeBuffer(currentBufSize));
}

/* One entry point for all next buffer size functions.
 * We split buffer sizes based on their smallest storage size.
 * It's faster to search one small target array with known limits rather
 * than having one array with 60 elements. */
size_t fibbufNextSizeBuffer(const size_t currentBufSize) {
    if (currentBufSize < 46368) {
        return fibbufNextBuffer16(currentBufSize);
    }

    /* On 32 bit systems we can't use fibbufNextBuffer64.
     * Also use 32-bit path for values below the first 64-bit fib,
     * allowing 20% growth fallback for values between last 32-bit fib
     * (2971215073) and first 64-bit fib (4807526976).
     * IMPORTANT: Don't use 32-bit path for values > UINT32_MAX to avoid
     * truncation when casting to uint32_t parameter. */
    if ((currentBufSize < 4807526976 && currentBufSize <= UINT32_MAX) ||
        sizeof(void *) == 4) {
        return fibbufNextBuffer32(currentBufSize);
    }

    return fibbufNextBuffer64(currentBufSize);
}

#define COUNT(array) (sizeof(array) / sizeof(*array))
#define FIBEND (fibbuf + COUNT(fibbuf) - 1)

#define BINARY_SEARCH_BRANCH_FREE                                              \
    do {                                                                       \
        /* binary search for the nearest fib */                                \
        do {                                                                   \
            mid = &result[half];                                               \
            result = (*mid < currentBufSize) ? mid : result;                   \
            n -= half;                                                         \
            half = n / 2;                                                      \
        } while (half > 0);                                                    \
    } while (0)

#define RETURN_MOSTLY_SAFE_RESULT                                              \
    do {                                                                       \
        if (*result > currentBufSize) {                                        \
            /* grew buffer size, ok */                                         \
            return *result;                                                    \
        }                                                                      \
                                                                               \
        /* result points to largest value < currentBufSize.                    \
         * We want the smallest fib value > currentBufSize. */                 \
        if ((result + 1) <= FIBEND) {                                          \
            if (*(result + 1) > currentBufSize) {                              \
                /* Next fib is larger, use it */                               \
                return *(result + 1);                                          \
            }                                                                  \
            /* *(result + 1) == currentBufSize (exact fib match).              \
             * We need to return the NEXT fib after the match. */              \
            if ((result + 2) <= FIBEND) {                                      \
                return *(result + 2);                                          \
            }                                                                  \
            /* Exact match at last element, fall through to 20% growth */      \
        }                                                                      \
                                                                               \
        /* The requested buffer size is beyond the extent of our               \
         * fib array, so just increase requested buffer by 20%. */             \
        /* Note: no check for overflow here, hence MOSTLY_SAFE. */             \
        return currentBufSize * 1.2;                                           \
    } while (0)

static uint16_t fibbufNextBuffer16(const uint16_t currentBufSize) {
    /* Note: we start at fib(9) == 34 because 34 bytes is a sane buffer size. */
    static const uint16_t fibbuf[] = {34,    55,    89,    144,  233,  377,
                                      610,   987,   1597,  2584, 4181, 6765,
                                      10946, 17711, 28657, 46368};
    static const size_t num = COUNT(fibbuf);

    const uint16_t *result = fibbuf;
    size_t n = num;
    size_t half = n / 2;
    const uint16_t *mid = NULL;

    BINARY_SEARCH_BRANCH_FREE;

    RETURN_MOSTLY_SAFE_RESULT;
}

static uint32_t fibbufNextBuffer32(const uint32_t currentBufSize) {
    /* Note: we start at fib(9) == 34 because 34 bytes is a sane buffer size. */
    static const uint32_t fibbuf[] = {
        /*        34,         55,         89,        144,       233,       377,
        610,        987,        1597,      2584,      4181,      6765,
        10946,      17711,      28657,     46368,*/
        75025,     121393,    196418,     317811,     514229,    832040,
        1346269,   2178309,   3524578,    5702887,    9227465,   14930352,
        24157817,  39088169,  63245986,   102334155,  165580141, 267914296,
        433494437, 701408733, 1134903170, 1836311903, 2971215073};
    static const size_t num = COUNT(fibbuf);

    const uint32_t *result = fibbuf;
    size_t n = num;
    size_t half = n / 2;
    const uint32_t *mid = NULL;

    BINARY_SEARCH_BRANCH_FREE;

    RETURN_MOSTLY_SAFE_RESULT;
}

static uint64_t fibbufNextBuffer64(const uint64_t currentBufSize) {
    /* Note: we start at fib(9) == 34 because 34 bytes is a sane buffer size. */
    static const uint64_t fibbuf[] =
        {
            /*        34,           55,           89,           144, 233, 377,
            610,          987,          1597,         2584, 4181,         6765,
            10946,        17711,        28657, 46368,        75025, 121393,
            196418,       317811, 514229,       832040,       1346269, 2178309,
            3524578, 5702887,      9227465,      14930352,     24157817,
            39088169, 63245986,     102334155,    165580141,    267914296,
            433494437, 701408733,    1134903170,   1836311903,   2971215073, */
            4807526976,   7778742049,   12586269025,  20365011074,
            32951280099,  53316291173,  86267571272,  139583862445,
            225851433717, 365435296162, 591286729879, 956722026041,
            1548008755920}; /* stops at a 1.54 TB buffer size */
    static const size_t num = COUNT(fibbuf);

    const uint64_t *result = fibbuf;
    size_t n = num;
    size_t half = n / 2;
    const uint64_t *mid = NULL;

    BINARY_SEARCH_BRANCH_FREE;

    RETURN_MOSTLY_SAFE_RESULT;
}

#ifdef DATAKIT_TEST

#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#include <inttypes.h> /* PRIu64 */

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

int fibbufTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("exact fibonacci boundary (regression for infinite loop bug)") {
        /* When input is exactly a fibonacci number, we must return the NEXT
         * fibonacci, not the same value. This caused infinite loops in code
         * that grows buffers like: while (size < minSize) size =
         * fibbufNext(size); */
        uint64_t next;

        /* Test all 16-bit fibonacci numbers */
        next = fibbufNextSizeBuffer(34);
        if (next != 55) {
            ERR("fibbuf(34) should be 55, got %" PRIu64, next);
        }

        next = fibbufNextSizeBuffer(377);
        if (next != 610) {
            ERR("fibbuf(377) should be 610, got %" PRIu64, next);
        }

        next = fibbufNextSizeBuffer(610);
        if (next != 987) {
            ERR("fibbuf(610) should be 987, got %" PRIu64, next);
        }

        next = fibbufNextSizeBuffer(46368);
        if (next != 75025) {
            ERR("fibbuf(46368) should be 75025, got %" PRIu64, next);
        }

        /* Test 32-bit fibonacci numbers */
        next = fibbufNextSizeBuffer(75025);
        if (next != 121393) {
            ERR("fibbuf(75025) should be 121393, got %" PRIu64, next);
        }

        /* 2971215073 is last element of 32-bit array, uses 20% growth */
        next = fibbufNextSizeBuffer(2971215073);
        if (next != (uint64_t)(2971215073 * 1.2)) {
            ERR("fibbuf(2971215073) should be 20%% growth, got %" PRIu64, next);
        }

        /* Test 64-bit fibonacci numbers */
        next = fibbufNextSizeBuffer(4807526976);
        if (next != 7778742049) {
            ERR("fibbuf(4807526976) should be 7778742049, got %" PRIu64, next);
        }

        /* Test last element - should use 20% growth */
        next = fibbufNextSizeBuffer(1548008755920);
        if (next != (uint64_t)(1548008755920 * 1.2)) {
            ERR("fibbuf(last) should be 20%% growth, got %" PRIu64, next);
        }
    }

    TEST("valid result 16") {
        uint64_t next = fibbufNextSizeBuffer(22);
        if (next != 34) {
            ERR("Not 34, but %" PRIu64 "!", next);
        }

        next = fibbufNextSizeBuffer(5000);
        if (next != 6765) {
            ERR("Not 6765, but %" PRIu64 "!", next);
        }

        next = fibbufNextSizeBuffer(50000);
        if (next != 75025) {
            ERR("Not 75025, but %" PRIu64 "!", next);
        }
    }

    TEST("valid result 32") {
        uint64_t next = fibbufNextSizeBuffer(22);
        if (next != 34) {
            ERR("Not 34, but %" PRIu64 "!", next);
        }

        next = fibbufNextSizeBuffer(72000);
        if (next != 75025) {
            ERR("Not 75025, but %" PRIu64 "!", next);
        }

        /* only grow by 20% since we're out of fib allocations. */
        next = fibbufNextSizeBuffer(2971215073 + 1);
        if (next != 3565458088) {
            ERR("Not 3565458088, but %" PRIu64 "!", next);
        }
    }

    TEST("valid result 64") {
        uint64_t next = fibbufNextSizeBuffer(22);
        if (next != 34) {
            ERR("Not 34, but %" PRIu64 "!", next);
        }

        next = fibbufNextSizeBuffer(72000);
        if (next != 75025) {
            ERR("Not 75025, but %" PRIu64 "!", next);
        }

        /* one whopper of a buffer */
        next = fibbufNextSizeBuffer(1548008755920 + 1);
        if (next != 1857610507105) {
            ERR("Not 1857610507105, but %" PRIu64 "!", next);
        }
    }

    const size_t loopers = 70000000;
    TEST("performance 16") {
        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(0);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 16 — 0");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(5000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 16 — 5000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(30000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 16 — 30000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(2178309 + 1);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 16 — 2178309 + 1");
        }
    }

    TEST("performance 32") {
        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(0);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 32 — 0");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(5000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 32 — 5000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(30000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 32 — 30000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(2178309 + 1);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 32 — 2178309 + 1");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(16777216);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 32 — 16777216");
        }
    }

    TEST("performance 64") {
        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(0);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 0");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(5000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 5000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(30000);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 30000");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(2178309 + 1);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 2178309 + 1");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(16777216);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 16777216");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopers; i++) {
                size_t next = fibbufNextSizeBuffer(139583862445);
                assert(next);
                (void)next;
            }
            TIME_FINISH(loopers, "perf 64 — 139583862445");
        }
    }

    TEST_FINAL_RESULT;
}

#endif
