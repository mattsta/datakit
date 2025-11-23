#include "timeUtil.h"

uint64_t timeUtilUs(void) {
    struct timeval tv;
    uint64_t ust;

    gettimeofday(&tv, NULL);
    ust = ((uint64_t)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

uint64_t timeUtilMs(void) {
    return timeUtilUs() / 1000;
}

uint64_t timeUtilS(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

/* This is a weird constructor because of the static 'unitOffset'
 * init. We don't want a race condition on init, so pre-init as constructor. */
#if DK_OS_APPLE
#define TIME_INIT __attribute__((constructor))
#else
#define TIME_INIT
#endif

/* We only care about an abstract monotonic time */
uint64_t TIME_INIT timeUtilMonotonicNs(void) {
#if DK_OS_APPLE_MAC
    /* on macOS, mach_absolute_time() returns time in nanoseconds directly */
    return mach_absolute_time();
#elif DK_OS_APPLE
    /* other Apple platforms don't guarantee the units
     * of mach_absolute_time, so we need to use internal
     * info to convert to final units */
    uint64_t machTime = mach_absolute_time();
    static mach_timebase_info_data_t timebaseInfo = {0};
    static uint64_t unitOffset = 0;

    if (!unitOffset) {
        (void)mach_timebase_info(&timebaseInfo);
        unitOffset = timebaseInfo.numer / timebaseInfo.denom;
    }

    return machTime * unitOffset;
#else
#if DK_OS_FREEBSD
    const clockid_t useClock = CLOCK_MONOTONIC_FAST;
#else
    const clockid_t useClock = CLOCK_MONOTONIC;
#endif
    /* else, we assume all other platforms have
     * at least CLOCK_MONOTONIC.
     * (e.g. openbsd, old linux, old freebsd, etc) */
    /* Note: also CLOCK_MONOTONIC_COARSE exists, but "COARSE" means it has about
     * a 1ms resolution and isn't very useful to us for generic timing. */
    struct timespec ts = {0};
    clock_gettime(useClock, &ts);
    const uint64_t us = (uint64_t)ts.tv_sec * (uint64_t)1e9;
    return us + ts.tv_nsec;
#endif /* platform clock picker */
}

uint64_t timeUtilMonotonicUs(void) {
    return timeUtilMonotonicNs() / 1000;
}

uint64_t timeUtilMonotonicMs(void) {
    return timeUtilMonotonicNs() / 1000000;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <unistd.h> /* usleep */

int timeUtilTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* Test that time values are non-zero and reasonable */
    printf("Testing basic time functions...\n");
    {
        uint64_t us = timeUtilUs();
        uint64_t ms = timeUtilMs();
        uint64_t s = timeUtilS();

        /* Times should be non-zero (we're past 1970) */
        if (us == 0) {
            ERR("timeUtilUs returned 0%s", "");
        }
        if (ms == 0) {
            ERR("timeUtilMs returned 0%s", "");
        }
        if (s == 0) {
            ERR("timeUtilS returned 0%s", "");
        }

        /* Sanity check: should be after year 2020 (1577836800 seconds) */
        if (s < 1577836800) {
            ERR("timeUtilS returned value before 2020: %" PRIu64, s);
        }

        /* Check that us/ms/s are consistent */
        uint64_t msFromUs = us / 1000;
        uint64_t sFromMs = ms / 1000;

        /* Allow 1 unit tolerance for timing differences */
        if (msFromUs < ms - 1 || msFromUs > ms + 1) {
            ERR("timeUtilUs (%" PRIu64 ") and timeUtilMs (%" PRIu64
                ") inconsistent",
                us, ms);
        }
        if (sFromMs < s || sFromMs > s + 1) {
            ERR("timeUtilMs (%" PRIu64 ") and timeUtilS (%" PRIu64
                ") inconsistent",
                ms, s);
        }
    }

    /* Test monotonic time increases */
    printf("Testing monotonic time increases...\n");
    {
        uint64_t ns1 = timeUtilMonotonicNs();
        uint64_t us1 = timeUtilMonotonicUs();
        uint64_t ms1 = timeUtilMonotonicMs();

        /* Small delay */
        usleep(1000); /* 1ms */

        uint64_t ns2 = timeUtilMonotonicNs();
        uint64_t us2 = timeUtilMonotonicUs();
        uint64_t ms2 = timeUtilMonotonicMs();

        /* Monotonic times should increase */
        if (ns2 <= ns1) {
            ERR("timeUtilMonotonicNs not monotonic: %" PRIu64 " -> %" PRIu64,
                ns1, ns2);
        }
        if (us2 < us1) {
            ERR("timeUtilMonotonicUs decreased: %" PRIu64 " -> %" PRIu64, us1,
                us2);
        }
        /* ms may not increase in 1ms */

        /* Check that the delta is reasonable (~1ms = 1000us = 1000000ns) */
        uint64_t deltaUs = us2 - us1;
        if (deltaUs < 500 || deltaUs > 100000) {
            /* Should be 1ms +/- tolerance, but allow wide margin */
            ERR("Unexpected time delta after 1ms sleep: %" PRIu64 " us",
                deltaUs);
        }
    }

    /* Test relationships between monotonic functions */
    printf("Testing monotonic time relationships...\n");
    {
        uint64_t ns = timeUtilMonotonicNs();
        uint64_t us = timeUtilMonotonicUs();
        uint64_t ms = timeUtilMonotonicMs();

        /* us should be approximately ns/1000 */
        uint64_t usFromNs = ns / 1000;
        int64_t diff = (int64_t)us - (int64_t)usFromNs;
        if (diff < -10 || diff > 10) {
            ERR("MonotonicNs (%" PRIu64 ") and MonotonicUs (%" PRIu64
                ") inconsistent",
                ns, us);
        }

        /* ms should be approximately us/1000 */
        uint64_t msFromUs = us / 1000;
        diff = (int64_t)ms - (int64_t)msFromUs;
        if (diff < -1 || diff > 1) {
            ERR("MonotonicUs (%" PRIu64 ") and MonotonicMs (%" PRIu64
                ") inconsistent",
                us, ms);
        }
    }

    /* Test that repeated calls don't return the same value */
    printf("Testing time resolution...\n");
    {
        uint64_t ns1 = timeUtilMonotonicNs();
        uint64_t ns2 = timeUtilMonotonicNs();
        uint64_t ns3 = timeUtilMonotonicNs();

        /* At least one pair should be different (shows time is advancing) */
        if (ns1 == ns2 && ns2 == ns3) {
            /* This might happen on very fast systems with coarse clocks */
            printf("Warning: monotonic ns returned same value 3 times "
                   "(clock resolution issue)\n");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
