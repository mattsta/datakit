/* monotonicCond - bounded condition-variable waits on a monotonic clock
 *
 * monotonicCondInit binds the condition variable to CLOCK_MONOTONIC
 * where pthread_condattr_setclock exists. Darwin lacks it, so there the
 * wait uses pthread_cond_timedwait_relative_np, which is unaffected by
 * wall-clock changes. A wait can return before the caller's condition
 * holds or its budget has passed (a spurious wake, or a budget clamped
 * to MONOTONIC_COND_MAX_WAIT_US), so callers loop on their own
 * predicate and deadline:
 *
 *   pthread_mutex_lock(&mu);
 *   while (!done && elapsedMs(start) < budgetMs) {
 *       monotonicCondWaitMs(&cv, &mu, budgetMs - elapsedMs(start));
 *   }
 *   pthread_mutex_unlock(&mu);
 */
#pragma once

#include "datakit.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#if defined(__APPLE__)
/* Darwin's relative condition wait. Its prototype is hidden by the
 * strict POSIX feature macros datakit compiles under. */
int pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                                       pthread_mutex_t *mutex,
                                       const struct timespec *reltime);
#endif

#define MONOTONIC_COND_NS_PER_SEC 1'000'000'000
#define MONOTONIC_COND_US_PER_SEC 1'000'000u
#define MONOTONIC_COND_MS_PER_SEC 1000u
#define MONOTONIC_COND_NS_PER_US 1000u
#define MONOTONIC_COND_US_PER_MS 1000u

/* Upper bound on one wait: 2^30 seconds (about 34 years). Larger budgets
 * are clamped to it, which keeps the budget-to-timespec conversion and
 * the absolute deadline inside even a 32-bit time_t. */
#define MONOTONIC_COND_MAX_WAIT_US                                             \
    ((UINT64_C(1) << 30) * (uint64_t)MONOTONIC_COND_US_PER_SEC)

/* Initialize a condition variable for monotonic bounded waits. */
static inline void monotonicCondInit(pthread_cond_t *cv) {
#if defined(__APPLE__)
    pthread_cond_init(cv, nullptr);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

/* One bounded wait of at most `budgetUs` microseconds. `mu` must be
 * held; it is released while waiting and re-held on return. Returns the
 * pthread result (0 on a wake, ETIMEDOUT on expiry); the caller decides
 * what to do next. Microseconds are the base unit so that sub-millisecond
 * budgets are not rounded up. */
static inline int monotonicCondWaitUs(pthread_cond_t *cv, pthread_mutex_t *mu,
                                      uint64_t budgetUs) {
    if (budgetUs > MONOTONIC_COND_MAX_WAIT_US) {
        budgetUs = MONOTONIC_COND_MAX_WAIT_US;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)(budgetUs / MONOTONIC_COND_US_PER_SEC);
    /* Below one second's worth of nanoseconds, so it fits tv_nsec. */
    ts.tv_nsec =
        (budgetUs % MONOTONIC_COND_US_PER_SEC) * MONOTONIC_COND_NS_PER_US;
#if defined(__APPLE__)
    return pthread_cond_timedwait_relative_np(cv, mu, &ts);
#else
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += ts.tv_sec;
    deadline.tv_nsec += ts.tv_nsec;
    if (deadline.tv_nsec >= MONOTONIC_COND_NS_PER_SEC) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= MONOTONIC_COND_NS_PER_SEC;
    }
    return pthread_cond_timedwait(cv, mu, &deadline);
#endif
}

/* monotonicCondWaitUs with a millisecond budget. */
static inline int monotonicCondWaitMs(pthread_cond_t *cv, pthread_mutex_t *mu,
                                      const uint64_t budgetMs) {
    if (budgetMs > MONOTONIC_COND_MAX_WAIT_US / MONOTONIC_COND_US_PER_MS) {
        return monotonicCondWaitUs(cv, mu, MONOTONIC_COND_MAX_WAIT_US);
    }
    return monotonicCondWaitUs(cv, mu, budgetMs * MONOTONIC_COND_US_PER_MS);
}
