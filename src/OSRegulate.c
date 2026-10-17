#include "OSRegulate.h"
#include "timeUtil.h"
#include "versionOSRuntime.h"

#include <errno.h>        /* strerror */
#include <fcntl.h>        /* open */
#include <limits.h>       /* INT_MAX (poll timeout width) */
#include <poll.h>         /* poll on a process-exit watch fd */
#include <stdio.h>        /* snprintf */
#include <sys/file.h>     /* flock */
#include <sys/resource.h> /* getrlimit */
#include <sys/wait.h>     /* waitpid / WNOHANG */
#include <unistd.h>       /* _exit */

/* rusage */
#include <sys/resource.h>
#include <sys/time.h>

#if DK_OS_LINUX
#include <sys/eventfd.h> /* OSRegulateWakeFdNew() */
#include <sys/prctl.h>   /* for OSRegulateRequestSignalChildWhenParentExits() */
#define OS_REGULATE_HAVE_EVENTFD 1
#endif

/* Process-exit event source: kqueue on the BSD family, pidfd on Linux,
 * none elsewhere. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||       \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define OS_REGULATE_HAVE_KQUEUE 1
#include <sys/event.h> /* kqueue / EVFILT_PROC / NOTE_EXIT */
#elif DK_OS_LINUX
#include <sys/syscall.h> /* SYS_pidfd_open */
#if defined(SYS_pidfd_open)
#define OS_REGULATE_HAVE_PIDFD 1
#endif
#endif

/* If errorMsg provided, set then return error.
 * If not provided, just return error */
#define returnError(m, e)                                                      \
    do {                                                                       \
        if (m) {                                                               \
            *(m) = (e);                                                        \
        }                                                                      \
        return OS_REGULATE_DAEMONIZE_ERROR;                                    \
    } while (0)

/* shorthand for returnError */
#define re(msg) returnError(errMsg, msg)

/* ====================================================================
 * Process Helpers
 * ==================================================================== */
OSRegulateDaemonizeStatus OSRegulateDaemonize(const char **errMsg) {
    switch (fork()) {
    case -1:
        /* error */
        re("Failure to fork() daemon");
    case 0:
        /* child */
        break;
    default:
        /* got pid of child, shutdown parent */
        return OS_REGULATE_DAEMONIZE_PARENT;
    }

    /* create new session id for child so it gets detached from parent */
    if (setsid() == -1) {
        re("Failed to run setsid()");
    }

    /* close default FDs (like daemonize()) */
    const int fd = open("/dev/null", O_RDWR, 0);
    if (fd == -1) {
        re("Failed to open /dev/null");
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        re("Failed to dup2 stdin");
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        re("Failed to dup2 stdout");
    }

    if (fd > STDERR_FILENO) {
        if (close(fd) == -1) {
            re("Failed to close /dev/null");
        }
    }

    return OS_REGULATE_DAEMONIZE_CHILD;
}

/* Wait for POLLIN on one descriptor until it is ready or `budgetMs` has
 * elapsed since `startMs` (timeUtilMonotonicMs).  An EINTR re-polls on
 * the time remaining, so signals cannot extend the wait, and a budget
 * wider than poll()'s int timeout is waited out in INT_MAX slices.
 * Returns the last poll() result: > 0 ready, 0 expired, < 0 failed with
 * errno set to something other than EINTR. */
static int osRegulatePollUntil(struct pollfd *pfd, const uint64_t startMs,
                               const uint64_t budgetMs) {
    for (;;) {
        const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
        const uint64_t remainingMs =
            elapsedMs >= budgetMs ? 0 : budgetMs - elapsedMs;
        const int timeoutMs =
            remainingMs > (uint64_t)INT_MAX ? INT_MAX : (int)remainingMs;
        const int n = poll(pfd, 1, timeoutMs);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n == 0 && remainingMs > (uint64_t)timeoutMs) {
            continue;
        }

        return n;
    }
}

/* The readiness channel is a stream socketpair rather than a pipe so a
 * late signal, sent after the reader has exited, cannot raise SIGPIPE:
 * sockets support MSG_NOSIGNAL / SO_NOSIGPIPE, pipes do not.  A stream
 * still carries both facts: one byte, and EOF once every writer closes. */
#if defined(MSG_NOSIGNAL)
#define OS_REGULATE_READY_SEND_FLAGS MSG_NOSIGNAL
#else
#define OS_REGULATE_READY_SEND_FLAGS 0
#endif

bool OSRegulateReadyPipeNew(int *outRead, int *outWrite) {
    if (!outRead || !outWrite) {
        return false;
    }
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    for (size_t i = 0; i < 2; i++) {
        const int fd = fcntl(fds[i], F_GETFD, 0);
        if (fd < 0 || fcntl(fds[i], F_SETFD, fd | FD_CLOEXEC) < 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
#if defined(SO_NOSIGPIPE)
        const int one = 1;
        if (setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) <
            0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
#endif
    }
    *outRead = fds[0];
    *outWrite = fds[1];
    return true;
}

void OSRegulateReadyPipeSignal(int *inoutWriteFd) {
    if (!inoutWriteFd || *inoutWriteFd < 0) {
        return;
    }
    const uint8_t one = 1;
    for (;;) {
        const ssize_t n = send(*inoutWriteFd, &one, sizeof(one),
                               OS_REGULATE_READY_SEND_FLAGS);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    /* Closing is part of the signal: the reader's end-of-file is the
     * "writer gone" fact, and a signalled writer has nothing more to
     * say. */
    close(*inoutWriteFd);
    *inoutWriteFd = -1;
}

OSRegulateReadyAwait OSRegulateReadyPipeAwaitMs(int readFd, uint64_t budgetMs) {
    if (readFd < 0) {
        return OS_REGULATE_READY_AWAIT_WRITER_GONE;
    }
    const uint64_t startMs = timeUtilMonotonicMs();
    struct pollfd want = {.fd = readFd, .events = POLLIN};
    for (;;) {
        const int n = osRegulatePollUntil(&want, startMs, budgetMs);
        if (n == 0) {
            return OS_REGULATE_READY_AWAIT_EXPIRED;
        }
        /* A poll failure means readiness can no longer be observed, so
         * it is reported as WRITER_GONE (not ready), as documented in
         * the header. */
        if (n < 0) {
            return OS_REGULATE_READY_AWAIT_WRITER_GONE;
        }
        /* Readable or hung up: one byte is the signal, end-of-file is
         * every writer gone.  A signal followed by the close reads the
         * byte first, so READY wins whenever it was sent. */
        uint8_t byte = 0;
        const ssize_t got = read(readFd, &byte, sizeof(byte));
        if (got > 0) {
            return OS_REGULATE_READY_AWAIT_READY;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return OS_REGULATE_READY_AWAIT_WRITER_GONE;
    }
}

/* The child's write end of the readiness channel, kept after the fork
 * so the child can signal from wherever it becomes ready. */
static int g_daemonizeReadyWriteFd = -1;

OSRegulateDaemonizeStatus OSRegulateDaemonizeUntilReady(uint64_t readyBudgetMs,
                                                        const char **errMsg) {
    int readyRead = -1;
    int readyWrite = -1;
    if (!OSRegulateReadyPipeNew(&readyRead, &readyWrite)) {
        re("Failed to create the readiness pipe");
    }
    const OSRegulateDaemonizeStatus status = OSRegulateDaemonize(errMsg);
    switch (status) {
    case OS_REGULATE_DAEMONIZE_ERROR:
        close(readyRead);
        close(readyWrite);
        return status;
    case OS_REGULATE_DAEMONIZE_CHILD:
        close(readyRead);
        g_daemonizeReadyWriteFd = readyWrite;
        return status;
    case OS_REGULATE_DAEMONIZE_PARENT:
        break;
    }
    /* The foreground parent: wait for the child's readiness, then exit
     * with a status a service manager can act on.  _exit() on both
     * paths: the parent must not run atexit handlers or flush stdio
     * buffers it shares with the child after the fork. */
    close(readyWrite);
    const OSRegulateReadyAwait verdict =
        OSRegulateReadyPipeAwaitMs(readyRead, readyBudgetMs);
    close(readyRead);
    if (verdict == OS_REGULATE_READY_AWAIT_READY) {
        OSRegulateExitClean();
    }
    _exit(EXIT_FAILURE);
}

void OSRegulateDaemonizeSignalReady(void) {
    OSRegulateReadyPipeSignal(&g_daemonizeReadyWriteFd);
}

void OSRegulateDaemonizeThenExitNoWait(void) {
    if (!OSRegulateDaemonize(NULL)) {
        /* parent process exits cleanly */
        OSRegulateExitClean();
    }

    /* child process continues */
}

bool OSRegulateWritePidToFile(const char *path) {
    FILE *fp = fopen(path, "w");
    if (fp) {
        /* Lock the pid so we aren't running twice */
        if (flock(fileno(fp), LOCK_EX | LOCK_NB) != 0) {
            return false;
        }

        fprintf(fp, "%d\n", (int)getpid());
        fflush(fp);

#if 0
        /* Don't close pid FD because we want to keep it locked */
        fclose(fp);
#endif

        return true;
    }

    return false;
}

void OSRegulateExitClean(void) {
    _exit(0);
    __builtin_unreachable();
}

bool OSRegulateRequestSignalChildWhenParentExits(int sig) {
#if DK_OS_LINUX
    /* We don't version check this because it has been available since
     * Linux 2.1.57 (1997-09-25) */
    return prctl(PR_SET_PDEATHSIG, sig) == 0;
#else
    (void)sig;
    return false;
#endif
}

bool OSRegulateParentStillExisits(void) {
    /* If parent is '1' we got reparented!
     * Obviously doesn't apply to daemons directly attached to init. */
    return !(getppid() == 1);
}

/* Process-exit events.  The watch fd is the only platform-specific
 * piece; the bounded await is portable code polling it. */
int OSRegulateProcessExitWatchFd(pid_t pid) {
#if defined(OS_REGULATE_HAVE_KQUEUE)
    /* A kqueue fd carrying a one-shot EVFILT_PROC/NOTE_EXIT filter is
     * itself select/poll-able, so it satisfies the pollable-fd contract
     * without the caller ever calling kevent(). */
    const int kq = kqueue();
    if (kq < 0) {
        return -1;
    }

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
           NULL);

    /* Register the filter (changelist of 1, no eventlist read yet).
     * ESRCH here means the process is already gone, so no event will
     * ever arrive: report "no event source" rather than a fd that never
     * becomes readable.  errno is preserved across the close because
     * the caller reads ESRCH as an exit report. */
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        const int armErrno = errno;
        close(kq);
        errno = armErrno;
        return -1;
    }

    return kq;
#elif defined(OS_REGULATE_HAVE_PIDFD)
    /* pidfd_open() has no libc wrapper on every supported glibc/musl
     * vintage, so go through the syscall directly.  The returned fd
     * becomes readable (POLLIN) when the process exits. */
    const int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        return -1;
    }

    return pidfd;
#else
    /* No kqueue and no pidfd: this platform offers no process-exit
     * event source, so callers must fall back to their own polling.
     * errno is set so a caller never reads a stale value as the arm's. */
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

bool OSRegulateProcessReapNow(pid_t pid, int *outStatus) {
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG);

    /* ECHILD: already reaped (or never ours); either way it is gone, and
     * its status is unknown, so 0 is reported. */
    if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
        if (outStatus) {
            *outStatus = status;
        }

        return true;
    }

    return false;
}

bool OSRegulateProcessAwaitExitMs(pid_t pid, uint64_t budgetMs,
                                  int *outStatus) {
    if (OSRegulateProcessReapNow(pid, outStatus)) {
        return true;
    }

    const uint64_t startMs = timeUtilMonotonicMs();
    bool exitReported = false;

    /* errno is cleared first so only an ESRCH from this arm counts. */
    errno = 0;
    const int watchFd = OSRegulateProcessExitWatchFd(pid);
    const int armErrno = errno;
    if (watchFd >= 0) {
        struct pollfd watch = {.fd = watchFd, .events = POLLIN};
        const int n = osRegulatePollUntil(&watch, startMs, budgetMs);
        exitReported = n > 0 && (watch.revents & POLLIN) != 0;

        close(watchFd);
    } else if (armErrno == ESRCH) {
        /* Arming fails with ESRCH once the kernel has begun tearing the
         * process down, which can precede the point where
         * waitpid(WNOHANG) reports it.  For an unreaped child the pid
         * cannot have been reused, so ESRCH counts as a reported exit. */
        exitReported = true;
    }

    if (exitReported) {
        /* The exit has been reported, so a blocking waitpid is bounded
         * by the kernel's in-progress exit; a WNOHANG reap can still
         * answer "running" for a moment after the notification. */
        int status = 0;
        pid_t reaped;
        while ((reaped = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
            /* retry */
        }
        if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
            if (outStatus) {
                *outStatus = status;
            }
            return true;
        }
        return false;
    }

    /* The budget expired or there was no event source.  Nothing has
     * reported an exit, so the final reap must not block. */
    return OSRegulateProcessReapNow(pid, outStatus);
}

/* ====================================================================
 * Wake descriptor
 * ==================================================================== */
bool OSRegulateWakeFdNew(int *outRead, int *outWrite) {
    if (!outRead || !outWrite) {
        return false;
    }
#if defined(OS_REGULATE_HAVE_EVENTFD)
    const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        return false;
    }
    *outRead = efd;
    *outWrite = efd;
    return true;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    for (size_t i = 0; i < 2; i++) {
        const int fl = fcntl(fds[i], F_GETFL, 0);
        const int fd = fcntl(fds[i], F_GETFD, 0);
        if (fl < 0 || fd < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            fcntl(fds[i], F_SETFD, fd | FD_CLOEXEC) < 0) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
    }
    *outRead = fds[0];
    *outWrite = fds[1];
    return true;
#endif
}

void OSRegulateWakeSignal(int writeFd) {
    if (writeFd < 0) {
        return;
    }
    /* Only async-signal-safe calls below: write(2) and errno accesses. */
    const int savedErrno = errno;
#if defined(OS_REGULATE_HAVE_EVENTFD)
    const uint64_t one = 1;
    (void)write(writeFd, &one, sizeof(one));
#else
    const uint8_t one = 1;
    (void)write(writeFd, &one, sizeof(one));
#endif
    errno = savedErrno;
}

void OSRegulateWakeDrain(int readFd) {
    if (readFd < 0) {
        return;
    }
#if defined(OS_REGULATE_HAVE_EVENTFD)
    uint64_t count = 0;
    (void)read(readFd, &count, sizeof(count));
#else
    uint8_t buf[64];
    while (read(readFd, buf, sizeof(buf)) > 0) {
        /* drain until EAGAIN */
    }
#endif
}

bool OSRegulateWakeAwaitMs(int readFd, uint64_t budgetMs) {
    if (readFd < 0) {
        return false;
    }
    struct pollfd want = {.fd = readFd, .events = POLLIN};
    if (osRegulatePollUntil(&want, timeUtilMonotonicMs(), budgetMs) > 0) {
        OSRegulateWakeDrain(readFd);
        return true;
    }

    return false;
}

/* ====================================================================
 * CPUs
 * ==================================================================== */
#include <sched.h>

size_t OSRegulateCPUCountGet(void) {
    ssize_t cpuCount = 1;
#if defined(_SC_NPROCESSORS_ONLN)
    cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
#endif

    // cppcheck-suppress knownConditionTrueFalse - sysconf can return -1 on
    // error in _SC_NPROCESSORS_ONLN path
    if (cpuCount == -1) {
        return 1;
    }

    return cpuCount;
}

/* ====================================================================
 * Networking
 * ==================================================================== */
/* Modes:
 *  - 1: Client
 *  - 2: Server
 *  - 3: Client and Server */
int OSRegulateTFOMode() {
    int tfo = 0;

    if (linuxKernelHasTFOServerIPv4) {
        FILE *fp = fopen("/proc/sys/net/ipv4/tcp_fastopen", "r");

        if (fp) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                tfo = atoi(buf);
            }

            fclose(fp);
        }
    }

    return tfo;
}

bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength) {
#if HAVE_PROC_SOMAXCONN
    FILE *fp = fopen("/proc/sys/net/core/somaxconn", "r");
    char buf[64] = {0};
    bool limitFits = true;

    if (!fp) {
        return false;
    }

    if (fgets(buf, sizeof(buf), fp) != NULL) {
        const int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < tcpBacklogListenLength) {
            limitFits = false;
        }
    }

    fclose(fp);
    return limitFits;
#else
    (void)tcpBacklogListenLength;
    return true;
#endif
}

/* ====================================================================
 * Files
 * ==================================================================== */
bool OSRegulateAdjustOpenFilesLimit(const size_t requestedFdCount,
                                    size_t *const limitActuallySet,
                                    char **statusMsg) {
    static char statusmsg[1024] = {0};

    /* Estimate of minimum FDs used internally before any user actions */
    static const rlim_t fdOverhead = 64;

    /* Number of fds to request from system */
    const rlim_t targetLimit = requestedFdCount + fdOverhead;

    if (statusMsg) {
        *statusMsg = statusmsg;
    }

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        snprintf(statusmsg, sizeof(statusmsg),
                 "Couldn't get current open file limit (%s)", strerror(errno));
        return false;
    }

    const rlim_t originalLimit = limit.rlim_cur;

    /* System limit is already greater than the number we're
     * requesting, so tell the caller what the current
     * number is and return success */
    if (originalLimit >= targetLimit) {
        if (limitActuallySet) {
            *limitActuallySet = originalLimit;
        }

        return true;
    }

    /* else, try to increase NOFILES ourself */
    rlim_t bestlimit = targetLimit;
    int setrlimit_error = 0;

    /* Try to set the file limit to match 'targetLimit' or at least
     * to the higher value supported less than targetLimit. */
    while (bestlimit > originalLimit) {
        const rlim_t decr_step = 16;

        limit.rlim_cur = bestlimit;
        limit.rlim_max = bestlimit;
        if (setrlimit(RLIMIT_NOFILE, &limit) != -1) {
            break;
        }

        setrlimit_error = errno;

        /* We failed to set file limit to 'bestlimit'. Try with a
         * smaller limit decrementing by a few FDs per iteration. */
        if (bestlimit < decr_step) {
            break;
        }

        bestlimit -= decr_step;
    }

    /* Assume that the limit we get initially is still valid if
     * our last try was even lower. */
    if (bestlimit < originalLimit) {
        bestlimit = originalLimit;
    }

    if (limitActuallySet) {
        *limitActuallySet = bestlimit;
    }

    if (bestlimit < targetLimit) {
        /* maxclients is unsigned so may overflow: in order
         * to check if maxclients is now logically less than 1
         * we test indirectly via bestlimit. */
        if (bestlimit <= fdOverhead) {
            snprintf(statusmsg, sizeof(statusmsg),
                     "Your current 'ulimit -n' "
                     "of %" PRIu64 " is not enough for the server to start. "
                     "Increase your open file limit to at least "
                     "%" PRIu64 ".",
                     (uint64_t)originalLimit, (uint64_t)targetLimit);
            return false;
        }

        snprintf(statusmsg, sizeof(statusmsg),
                 "Server can't set maximum open files "
                 "to %" PRIu64 " because of OS error: %s.",
                 (uint64_t)targetLimit, strerror(setrlimit_error));

        /* 'false' because we changed limit,
         * BUT not as much as requested. */
        return false;
    }

    snprintf(statusmsg, sizeof(statusmsg),
             "Increased maximum number of open files "
             "to %" PRIu64 " (was originally %" PRIu64 ").",
             (uint64_t)targetLimit, (uint64_t)originalLimit);

    /* 'true' because we successfully changed limit. */
    return true;
}

/* ====================================================================
 * Usage
 * ==================================================================== */
bool OSRegulateResourceUsageGet(long *selfMaxRSS, long *childMaxRSS,
                                float *selfSystemCPU, float *selfUserCPU,
                                float *childSystemCPU, float *childUserCPU) {
    struct rusage self;
    struct rusage children;
    if (getrusage(RUSAGE_SELF, &self) != 0) {
        return false;
    }

    if (getrusage(RUSAGE_CHILDREN, &children) != 0) {
        return false;
    }

#if DK_OS_APPLE
    /* OS X reports size in bytes */
    *selfMaxRSS = self.ru_maxrss;
    *childMaxRSS = children.ru_maxrss;
#else
    /* Linux and FreeBSD report size in KB */
    *selfMaxRSS = self.ru_maxrss * 1024ULL;
    *childMaxRSS = children.ru_maxrss * 1024ULL;
#endif

    *selfSystemCPU =
        (float)self.ru_stime.tv_sec + (float)self.ru_stime.tv_usec / 1000000;

    *selfUserCPU =
        (float)self.ru_utime.tv_sec + (float)self.ru_utime.tv_usec / 1000000;

    *childSystemCPU = (float)children.ru_stime.tv_sec +
                      (float)children.ru_stime.tv_usec / 1000000;

    *childUserCPU = (float)children.ru_utime.tv_sec +
                    (float)children.ru_utime.tv_usec / 1000000;

    return true;
}

/* ====================================================================
 * Memory
 * ==================================================================== */
#if DK_OS_LINUX
bool OSRegulateLinuxOvercommitEnabled(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory", "r");
    char buf[64] = {0};

    if (!fp) {
        return false;
    }

    if (fgets(buf, 64, fp) == NULL) {
        fclose(fp);
        return false;
    }

    fclose(fp);

    const int overcommitSetting = atoi(buf);

    /* Ovecommit of 0 or 1 is enabled, 2 is disabled. */
    return overcommitSetting <= 1;
}

bool OSRegulateLinuxTransparentHugePagesEnabled(void) {
    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char buf[1024] = {0};

    if (!fp) {
        return false;
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        return false;
    }

    fclose(fp);

    /* If '[never]' is selected, 'strstr' finds it
     * and returns a value (so if found, setting is disabled),
     * else, '[never]' isn't found meaning THP is enabled,
     * so return true if NOT found (meaning enabled) and
     *    return false if FOUND (meaning disabled) */
    return strstr(buf, "[never]") == NULL;
}

bool OSRegulateLinuxMemorySettingsAreOkay(void) {
    if (OSRegulateLinuxOvercommitEnabled() &&
        !OSRegulateLinuxTransparentHugePagesEnabled()) {
        return true;
    }

    return false;
}

/* Get the sum of the specified field (converted form kb to bytes) in
 * /proc/self/smaps. The field must be specified with trailing ":" as it
 * apperas in the smaps output.
 *
 * If a pid is specified, the information is extracted for such a pid,
 * otherwise if pid is -1 the information is reported is about the
 * current process.
 *
 * Example: zmalloc_get_smap_bytes_by_field("Rss:",-1);
 */
size_t OSRegulateLinuxSmapBytesByFieldForPid(const char *field, int64_t pid) {
#if HAVE_PROC_SMAPS
    char line[1024] = {0};
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1) {
        fp = fopen("/proc/self/smaps", "r");
    } else {
        char filename[128];
        snprintf(filename, sizeof(filename), "/proc/%ld/smaps", pid);
        fp = fopen(filename, "r");
    }

    if (!fp) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, field, flen) == 0) {
            char *const p = strchr(line, 'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line + flen, NULL, 10) * 1024;
            }
        }
    }

    fclose(fp);
    return bytes;
#else
    (void)field;
    (void)pid;
    return 0;
#endif
}

size_t OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(int64_t pid) {
    return OSRegulateLinuxSmapBytesByFieldForPid("AnonHugePages:", pid);
}

size_t OSRegualteLinuxSmapPrivateDirtyGet(int64_t pid) {
    return OSRegulateLinuxSmapBytesByFieldForPid("Private_Dirty:", pid);
}
#endif

#if defined(HAVE_PROC_STAT)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd;
    int count;
    char *p;
    char *x;

    snprintf(filename, 256, "/proc/%d/stat", getpid());
    if ((fd = open(filename, O_RDONLY)) == -1) {
        return 0;
    }

    if (read(fd, buf, 4096) <= 0) {
        close(fd);
        return 0;
    }

    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while (p && count--) {
        p = strchr(p, ' ');
        if (p) {
            p++;
        }
    }

    if (!p) {
        return 0;
    }

    x = strchr(p, ' ');
    if (!x) {
        return 0;
    }

    *x = '\0';

    rss = strtoll(p, NULL, 10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <mach/mach_init.h>
#include <mach/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info = {0};
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS) {
        return 0;
    }

    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t OSRegulateRSSGet(void) {
    return -1;
}
#endif

/* Returns the size of physical memory (RAM) in bytes.
 * It looks ugly, but this is the cleanest way to achive cross platform results.
 * Cleaned up from:
 *
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * Note that this function:
 * 1) Was released under the following CC attribution license:
 *    http://creativecommons.org/licenses/by/3.0/deed.en_US.
 * 2) Was originally implemented by David Robert Nadeau.
 * 3) Was modified by Matt Stancliff.
 * 4) This note exists in order to comply with the original license.
 */
size_t OSRegulateTotalMemoryGet(void) {
#if defined(__unix__) || defined(__unix) || defined(unix) ||                   \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2] = {0};
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE; /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64; /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0; /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0) {
        return (size_t)size;
    }
    return 0; /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, and OpenBSD. ----------------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2] = {0};
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM; /* FreeBSD. ----------------- */
#elif defined(HW_PYSMEM)
    mib[1] = HW_PHYSMEM; /* Others. ------------------ */
#endif
    uint32_t size = 0; /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0) {
        return (size_t)size;
    }

    return 0; /* Failed? */
#else
    return 0; /* Unknown method to get the data. */
#endif
#else
    return 0; /* Unknown OS. */
#endif
}

/* ====================================================================
 * Network Validators
 * ==================================================================== */
/* RFC 6890 notes special IP spaces in general */
/* RFC 1918 specifies IPv4 special addresses */
/* RFC 4193 specifies IPv6 special addresses */
/* We're ignoring some of the more esoteric "benchmarking" networks for now. */
bool OSRegulateNetworkIsPrivate(const struct addrinfo *ai) {
    if (ai->ai_family == AF_INET) {
        return OSRegulateNetworkIsPrivateIPv4(
            (const struct sockaddr_in *)ai->ai_addr);
    }

    return OSRegulateNetworkIsPrivateIPv6(
        (const struct sockaddr_in6 *)ai->ai_addr);
}

bool OSRegulateNetworkIsPrivateIPv4(const struct sockaddr_in *const sa) {
    const uint8_t *netAddr = (uint8_t *)&sa->sin_addr.s_addr;

    /* The address integer in 'sa' is big endian. */

    /* RFC 1918 */
    /* 127.0.0.0/8 */
    if (netAddr[0] == 127) {
        return true;
    }

    /* 192.168.0.0/16 */
    if (netAddr[0] == 192 && netAddr[1] == 168) {
        return true;
    }

    /* 172.16.0.0/12 */
    if (netAddr[0] == 172 && (netAddr[1] >= 16 && netAddr[1] <= 31)) {
        return true;
    }

    return false;
}

bool OSRegulateNetworkIsPrivateIPv6(const struct sockaddr_in6 *const sa6) {
    const uint8_t *netAddr = sa6->sin6_addr.s6_addr;

    /* RFC 4193 section 3: Local IPv6 Unicast Addresses */
    /* fc00::/7 */
    if (netAddr[0] == 0xfc || netAddr[0] == 0xfd) {
        return true;
    }

    /* RFC 4291 section 2.5.3: The Loopback Address */
    /* ::1/128 */
    /* We have to verify the first 15 bytes are zero and the final
     * byte is just 1 */
    bool is1 = true;
    for (size_t i = 0; i < 15; i++) {
        if (netAddr[i] != 0) {
            is1 = false;
            break;
        }
    }

    if (is1 && netAddr[15] == 1) {
        return true;
    }

    /* RFC 4291 section 2.5.6: Link-Local IPv6 Unicast Addresses */
    /* fe80::/10 */
    if (netAddr[0] == 0xfe && (netAddr[1] >= 0x80 && netAddr[1] <= 0xbf)) {
        return true;
    }

    return false;
}

bool OSRegulateNetworkIsAll(const struct addrinfo *ai) {
    if (ai->ai_family == AF_INET) {
        return OSRegulateNetworkIsAllIPv4(
            (const struct sockaddr_in *)ai->ai_addr);
    }

    return OSRegulateNetworkIsAllIPv6((const struct sockaddr_in6 *)ai->ai_addr);
}

bool OSRegulateNetworkIsAllIPv4(const struct sockaddr_in *const sa) {
    const uint32_t netAddr = sa->sin_addr.s_addr;

    if (netAddr != 0) {
        return false;
    }

    return true;
}

bool OSRegulateNetworkIsAllIPv6(const struct sockaddr_in6 *const sa6) {
    const uint8_t *netAddr = sa6->sin6_addr.s6_addr;
    for (size_t i = 0; i < 16; i++) {
        if (netAddr[i] != 0) {
            return false;
        }
    }

    return true;
}
