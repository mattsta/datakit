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
