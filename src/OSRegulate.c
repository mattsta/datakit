#include "OSRegulate.h"
#include "cleaner.h"
#include "timeUtil.h"
#include "versionOSRuntime.h"

#include <errno.h>        /* strerror */
#include <fcntl.h>        /* open */
#include <limits.h>       /* INT_MAX (poll timeout width) */
#include <poll.h>         /* poll on a process-exit watch fd */
#include <stdio.h>        /* snprintf */
#include <stdlib.h>       /* strtoull */
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

/* pipe2() sets O_NONBLOCK and O_CLOEXEC atomically with creation.  Linux
 * (glibc 2.9+, musl) and the BSDs have it; macOS does not, and falls
 * back to fcntl() after pipe().  Linux uses an eventfd for the wake
 * descriptor, so only the BSDs reach the pipe2() path. */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||     \
    defined(__DragonFly__)
#define OS_REGULATE_HAVE_PIPE2 1
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
    cleaner(fdClose) int fd = open("/dev/null", O_RDWR, 0);
    if (fd == -1) {
        re("Failed to open /dev/null");
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        re("Failed to dup2 stdin");
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        re("Failed to dup2 stdout");
    }

    /* With a standard stream already closed, /dev/null itself landed on
     * that stream's number and is now the stream: keep it. */
    if (fd <= STDERR_FILENO) {
        (void)fdRelease(&fd);
    } else if (close(fdRelease(&fd)) == -1) {
        re("Failed to close /dev/null");
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

/* Set close-on-exec on a descriptor that could not get it atomically at
 * creation.  Platforms with SOCK_CLOEXEC / pipe2() never call this, so
 * only they close the window in which another thread's fork+exec
 * inherits the descriptor. */
DK_FN_UNUSED static bool osRegulateSetCloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) >= 0;
}

/* Move a descriptor above the standard streams, keeping it
 * close-on-exec.  A process started with stdin or stdout closed gets
 * those numbers back from the next socketpair(), and the dup2() of
 * /dev/null onto the streams in OSRegulateDaemonize() would then
 * silently close the channel. */
static bool osRegulateFdAboveStdio(int *fd) {
    if (*fd > STDERR_FILENO) {
        return true;
    }

    const int moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        return false;
    }

    close(*fd);
    *fd = moved;
    return true;
}

bool OSRegulateReadyPipeNew(int *outRead, int *outWrite) {
    if (!outRead || !outWrite) {
        return false;
    }
    int fds[2];
#if defined(SOCK_CLOEXEC)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        return false;
    }
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
#endif
    cleaner(fdClose) int rd = fds[0];
    cleaner(fdClose) int wr = fds[1];
#if !defined(SOCK_CLOEXEC)
    if (!osRegulateSetCloexec(rd) || !osRegulateSetCloexec(wr)) {
        return false;
    }
#endif
    if (!osRegulateFdAboveStdio(&rd) || !osRegulateFdAboveStdio(&wr)) {
        return false;
    }
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    if (setsockopt(rd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0 ||
        setsockopt(wr, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
        return false;
    }
#endif
    *outRead = fdRelease(&rd);
    *outWrite = fdRelease(&wr);
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
    cleaner(fdClose) int readyRead = -1;
    cleaner(fdClose) int readyWrite = -1;
    if (!OSRegulateReadyPipeNew(&readyRead, &readyWrite)) {
        re("Failed to create the readiness pipe");
    }
    const OSRegulateDaemonizeStatus status = OSRegulateDaemonize(errMsg);
    switch (status) {
    case OS_REGULATE_DAEMONIZE_ERROR:
        return status;
    case OS_REGULATE_DAEMONIZE_CHILD:
        g_daemonizeReadyWriteFd = fdRelease(&readyWrite);
        return status;
    case OS_REGULATE_DAEMONIZE_PARENT:
        break;
    }
    /* The foreground parent: wait for the child's readiness, then exit
     * with a status a service manager can act on.  _exit() on both
     * paths: the parent must not run atexit handlers or flush stdio
     * buffers it shares with the child after the fork.  The write end is
     * closed before the wait so the child's end-of-file can be seen. */
    close(fdRelease(&readyWrite));
    const OSRegulateReadyAwait verdict =
        OSRegulateReadyPipeAwaitMs(readyRead, readyBudgetMs);
    close(fdRelease(&readyRead));
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
    /* Open without truncating: the file may belong to a running instance,
     * and only the lock holder may rewrite it.  Close-on-exec, so an
     * exec'd child cannot keep the lock alive after this process exits. */
    cleaner(fdClose) int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }

    /* Lock the pid so we aren't running twice */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        return false;
    }

    char line[32];
    const int len = snprintf(line, sizeof(line), "%d\n", (int)getpid());
    if (len <= 0 || (size_t)len >= sizeof(line) || ftruncate(fd, 0) != 0) {
        return false;
    }

    for (int written = 0; written < len;) {
        const ssize_t n = write(fd, line + written, (size_t)(len - written));
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return false;
        }

        written += (int)n;
    }

    /* The descriptor stays open for the life of the process: closing it
     * would release the lock. */
    (void)fdRelease(&fd);
    return true;
}

[[noreturn]] void OSRegulateExitClean(void) {
    _exit(0);
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
    /* 0 and negative pids name process groups or "any process" to the
     * kernel, never one process. */
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }

#if defined(OS_REGULATE_HAVE_KQUEUE)
    /* A kqueue fd carrying a one-shot EVFILT_PROC/NOTE_EXIT filter is
     * itself select/poll-able, so it satisfies the pollable-fd contract
     * without the caller ever calling kevent(). */
    cleaner(fdClose) int kq = kqueue();
    if (kq < 0) {
        return -1;
    }

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
           NULL);

    /* Register the filter (changelist of 1, no eventlist read yet).
     * ESRCH here means the process is already gone, so no event will
     * ever arrive: report "no event source" rather than a fd that never
     * becomes readable.  The scope-exit close preserves errno, because
     * the caller reads ESRCH as an exit report. */
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        return -1;
    }

    return fdRelease(&kq);
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
    /* waitpid() reads 0 and negative pids as "any child" or a process
     * group: such a reap would take an unrelated child's status, and
     * the 0 return for "none ready" would equal pid 0 and read as
     * reaped. */
    if (pid <= 0) {
        errno = EINVAL;
        return false;
    }

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

/* Reap `pid` with non-blocking attempts until it is reaped or `budgetMs`
 * has elapsed since `startMs` (timeUtilMonotonicMs).  This is the wait
 * when no exit notification can be armed (descriptor exhaustion, or a
 * kernel or sandbox refusing pidfd_open), so the budget still holds.
 * The sleep between attempts starts at 1 ms and doubles up to 50 ms,
 * and is cut to the time remaining so it never overshoots the deadline;
 * a signal cutting a sleep short only brings the next attempt forward. */
static bool osRegulateReapPollUntil(pid_t pid, const uint64_t startMs,
                                    const uint64_t budgetMs, int *outStatus) {
    static constexpr uint64_t maxStepMs = 50;
    uint64_t stepMs = 1;
    for (;;) {
        if (OSRegulateProcessReapNow(pid, outStatus)) {
            return true;
        }

        const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
        if (elapsedMs >= budgetMs) {
            return false;
        }

        const uint64_t remainingMs = budgetMs - elapsedMs;
        const uint64_t sleepMs = stepMs < remainingMs ? stepMs : remainingMs;
        (void)poll(NULL, 0, (int)sleepMs);

        stepMs = stepMs * 2 < maxStepMs ? stepMs * 2 : maxStepMs;
    }
}

bool OSRegulateProcessAwaitExitMs(pid_t pid, uint64_t budgetMs,
                                  int *outStatus) {
    /* See ReapNow: 0 and negative pids never name one child. */
    if (pid <= 0) {
        errno = EINVAL;
        return false;
    }

    if (OSRegulateProcessReapNow(pid, outStatus)) {
        return true;
    }

    const uint64_t startMs = timeUtilMonotonicMs();
    bool exitReported = false;

    /* errno is cleared first so only an ESRCH from this arm counts. */
    errno = 0;
    cleaner(fdClose) int watchFd = OSRegulateProcessExitWatchFd(pid);
    const int armErrno = errno;
    if (watchFd >= 0) {
        struct pollfd watch = {.fd = watchFd, .events = POLLIN};
        const int n = osRegulatePollUntil(&watch, startMs, budgetMs);
        if (n < 0) {
            /* The notification can no longer be observed; the budget
             * still holds. */
            return osRegulateReapPollUntil(pid, startMs, budgetMs, outStatus);
        }

        exitReported = n > 0 && (watch.revents & POLLIN) != 0;
    } else if (armErrno == ESRCH) {
        /* Arming fails with ESRCH once the kernel has begun tearing the
         * process down, which can precede the point where
         * waitpid(WNOHANG) reports it.  For an unreaped child the pid
         * cannot have been reused, so ESRCH counts as a reported exit. */
        exitReported = true;
    } else {
        /* No notification could be armed: no event source on this
         * platform, or a run-time refusal (EMFILE, ENFILE, ENOMEM, or
         * ENOSYS / EPERM from an older kernel or a sandbox).  Polling
         * keeps the caller's budget instead of giving up at once. */
        return osRegulateReapPollUntil(pid, startMs, budgetMs, outStatus);
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

    /* The budget expired.  Nothing has reported an exit, so the final
     * reap must not block. */
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
#elif defined(OS_REGULATE_HAVE_PIPE2)
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        return false;
    }
    *outRead = fds[0];
    *outWrite = fds[1];
    return true;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    cleaner(fdClose) int rd = fds[0];
    cleaner(fdClose) int wr = fds[1];
    const int ends[2] = {rd, wr};
    for (size_t i = 0; i < 2; i++) {
        const int fl = fcntl(ends[i], F_GETFL, 0);
        if (fl < 0 || fcntl(ends[i], F_SETFL, fl | O_NONBLOCK) < 0 ||
            !osRegulateSetCloexec(ends[i])) {
            return false;
        }
    }
    *outRead = fdRelease(&rd);
    *outWrite = fdRelease(&wr);
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

/* What a drain found: a wake consumed, nothing pending, or (pipe only)
 * end-of-file because every write end is closed. */
typedef enum osRegulateWakeDrained {
    OS_REGULATE_WAKE_DRAINED_WAKE,
    OS_REGULATE_WAKE_DRAINED_EMPTY,
    OS_REGULATE_WAKE_DRAINED_HUNG_UP,
} osRegulateWakeDrained;

static osRegulateWakeDrained osRegulateWakeDrainFd(int readFd) {
#if defined(OS_REGULATE_HAVE_EVENTFD)
    uint64_t count = 0;
    return read(readFd, &count, sizeof(count)) == (ssize_t)sizeof(count)
               ? OS_REGULATE_WAKE_DRAINED_WAKE
               : OS_REGULATE_WAKE_DRAINED_EMPTY;
#else
    bool woke = false;
    uint8_t buf[64];
    for (;;) {
        const ssize_t got = read(readFd, buf, sizeof(buf));
        if (got > 0) {
            woke = true;
            continue;
        }

        if (got < 0 && errno == EINTR) {
            continue;
        }

        /* EAGAIN once drained; 0 is end-of-file */
        if (woke) {
            return OS_REGULATE_WAKE_DRAINED_WAKE;
        }

        return got == 0 ? OS_REGULATE_WAKE_DRAINED_HUNG_UP
                        : OS_REGULATE_WAKE_DRAINED_EMPTY;
    }
#endif
}

void OSRegulateWakeDrain(int readFd) {
    if (readFd < 0) {
        return;
    }

    (void)osRegulateWakeDrainFd(readFd);
}

bool OSRegulateWakeAwaitMs(int readFd, uint64_t budgetMs) {
    if (readFd < 0) {
        return false;
    }

    const uint64_t startMs = timeUtilMonotonicMs();
    struct pollfd want = {.fd = readFd, .events = POLLIN};
    for (;;) {
        const int n = osRegulatePollUntil(&want, startMs, budgetMs);
        if (n <= 0) {
            return false;
        }

        /* Not a descriptor poll() can wait on (closed, or an error
         * condition): a poll() failure, as the header documents. */
        if (want.revents & (POLLNVAL | POLLERR)) {
            return false;
        }

        osRegulateWakeDrained drained = OS_REGULATE_WAKE_DRAINED_EMPTY;
        if (want.revents & POLLIN) {
            drained = osRegulateWakeDrainFd(readFd);
            if (drained == OS_REGULATE_WAKE_DRAINED_WAKE) {
                return true;
            }
        }

        if (drained == OS_REGULATE_WAKE_DRAINED_HUNG_UP ||
            (want.revents & POLLHUP)) {
            /* Every write end is closed, so no wake can arrive, and
             * poll() would report the hang-up at once forever.  Wait
             * out the budget as an idle await would, so a caller
             * looping on the await does not spin; a negative fd makes
             * poll() a plain timed sleep. */
            struct pollfd none = {.fd = -1};
            (void)osRegulatePollUntil(&none, startMs, budgetMs);
            return false;
        }

        /* Readable but already drained (another consumer won the
         * race): wait on the time remaining. */
    }
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
 * Kernel value parsing
 * ==================================================================== */
/* Parse the unsigned decimal integer a /proc or /sys file holds in 's':
 * digits with optional surrounding blanks and a trailing newline.  Writes
 * *value and returns true only when that is all of 's' and the value is at
 * most 'max'; a sign, an empty string, any other character, or a value
 * past 'max' (or past UINT64_MAX) returns false and leaves *value alone. */
DK_FN_UNUSED static bool osrParseKernelU64_(const char *s, uint64_t max,
                                            uint64_t *value) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }

    /* strtoull would accept a sign (and negate "-1" into a huge value) */
    if (*s < '0' || *s > '9') {
        return false;
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

/* The resident set size, in pages, from the text of a /proc/<pid>/stat
 * file: field 24.  Field 2 (comm) is the executable name in parentheses
 * and may itself hold spaces and ')', so fields are counted from the
 * LAST ')' in the line, after which every field is a space-separated
 * token.  Writes *pages and returns true only when field 24 is an
 * unsigned decimal of at most 'maxPages'. */
DK_FN_UNUSED static bool
osrProcStatRssPages_(const char *stat, uint64_t maxPages, uint64_t *pages) {
    const char *p = strrchr(stat, ')');
    if (!p) {
        return false;
    }

    p++;
    for (int field = 3; field <= 24; field++) {
        if (*p != ' ') {
            return false;
        }

        p++;
        const char *const start = p;
        while (*p != '\0' && *p != ' ' && *p != '\n') {
            p++;
        }

        if (p == start) {
            return false;
        }

        if (field == 24) {
            char digits[24];
            const size_t n = (size_t)(p - start);
            if (n >= sizeof(digits)) {
                return false;
            }

            memcpy(digits, start, n);
            digits[n] = '\0';
            return osrParseKernelU64_(digits, maxPages, pages);
        }
    }

    return false;
}

/* ====================================================================
 * Networking
 * ==================================================================== */
/* Modes:
 *  - 1: Client
 *  - 2: Server
 *  - 3: Client and Server */
int OSRegulateTFOMode(void) {
    int tfo = 0;

    /* The kernel version is a Linux one only on Linux */
    if (DK_OS_LINUX && linuxKernelHasTFOServerIPv4) {
        FILE *fp = fopen("/proc/sys/net/ipv4/tcp_fastopen", "r");

        if (fp) {
            char buf[64] = {};
            uint64_t mode = 0;
            /* An unparsable value reads as TFO off */
            if (fgets(buf, sizeof(buf), fp) != NULL &&
                osrParseKernelU64_(buf, INT_MAX, &mode)) {
                tfo = (int)mode;
            }

            fclose(fp);
        }
    }

    return tfo;
}

/* Whether a listen() backlog of 'requested' fits the somaxconn limit the
 * kernel file holds as 'contents', or NULL when the file could not be
 * read.  A limit that cannot be read or parsed cannot be checked, so it
 * does not fit; a limit of 0 or a requested length of 0 or less imposes
 * nothing, so it fits. */
DK_FN_UNUSED static bool osrBacklogFits_(const char *contents, int requested) {
    uint64_t somaxconn = 0;
    if (!contents || !osrParseKernelU64_(contents, INT_MAX, &somaxconn)) {
        return false;
    }

    return somaxconn == 0 || requested <= 0 || somaxconn >= (uint64_t)requested;
}

bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength) {
#if DK_OS_LINUX /* /proc/sys/net/core/somaxconn */
    FILE *fp = fopen("/proc/sys/net/core/somaxconn", "r");
    if (!fp) {
        return osrBacklogFits_(NULL, tcpBacklogListenLength);
    }

    char buf[64] = {};
    const bool readOk = fgets(buf, sizeof(buf), fp) != NULL;
    fclose(fp);
    return osrBacklogFits_(readOk ? buf : NULL, tcpBacklogListenLength);
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
    static char statusmsg[1024] = {};

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
bool OSRegulateResourceUsageGet(int64_t *selfMaxRSS, int64_t *childMaxRSS,
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
    *selfMaxRSS = (int64_t)self.ru_maxrss * 1024;
    *childMaxRSS = (int64_t)children.ru_maxrss * 1024;
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
    char buf[64] = {};

    if (!fp) {
        return false;
    }

    if (fgets(buf, 64, fp) == NULL) {
        fclose(fp);
        return false;
    }

    fclose(fp);

    uint64_t overcommitSetting = 0;
    if (!osrParseKernelU64_(buf, INT_MAX, &overcommitSetting)) {
        return false; /* unparsable, as unreadable */
    }

    /* Ovecommit of 0 or 1 is enabled, 2 is disabled. */
    return overcommitSetting <= 1;
}

bool OSRegulateLinuxTransparentHugePagesEnabled(void) {
    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char buf[1024] = {};

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
#if DK_OS_LINUX /* /proc/<pid>/smaps */
    char line[1024] = {};
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1) {
        fp = fopen("/proc/self/smaps", "r");
    } else {
        char filename[128];
        snprintf(filename, sizeof(filename), "/proc/%" PRId64 "/smaps", pid);
        fp = fopen(filename, "r");
    }

    if (!fp) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, field, flen) == 0) {
            /* "<field>   <n> kB": search past the field name, which may
             * hold a 'k' itself (as "Locked:" does) */
            char *const p = strchr(line + flen, 'k');
            if (p) {
                *p = '\0';
                uint64_t kb = 0;
                size_t lineBytes = 0;
                if (!osrParseKernelU64_(line + flen, SIZE_MAX, &kb) ||
                    ckd_mul(&lineBytes, kb, (size_t)1024) ||
                    ckd_add(&bytes, bytes, lineBytes)) {
                    /* A malformed or impossible line: the sum is unknown */
                    fclose(fp);
                    return 0;
                }
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

#if DK_OS_LINUX /* /proc/<pid>/stat */
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    const int64_t page = sysconf(_SC_PAGESIZE);
    char buf[4096];
    char filename[256];
    int fd;

    if (page <= 0) {
        return 0;
    }

    snprintf(filename, 256, "/proc/%d/stat", getpid());
    if ((fd = open(filename, O_RDONLY)) == -1) {
        return 0;
    }

    /* Leave room to terminate: the field scan uses string functions */
    const ssize_t got = read(fd, buf, sizeof(buf) - 1);
    if (got <= 0) {
        close(fd);
        return 0;
    }

    close(fd);
    buf[got] = '\0';

    uint64_t pages = 0;
    if (!osrProcStatRssPages_(buf, SIZE_MAX / (size_t)page, &pages)) {
        return 0; /* malformed stat line: RSS unknown */
    }

    return (size_t)pages * (size_t)page;
}
#elif DK_OS_APPLE /* task_info() */
#include <mach/mach_init.h>
#include <mach/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

size_t OSRegulateRSSGet(void) {
    /* mach_task_self() is this task's cached send right; unlike a
     * task_for_pid() lookup it takes no new port reference to release */
    struct mach_task_basic_info info = {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &count) != KERN_SUCCESS) {
        return 0;
    }

    return (size_t)info.resident_size;
}
#else
size_t OSRegulateRSSGet(void) {
    return 0; /* unknown, as on any error */
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
    int mib[2] = {};
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
    int mib[2] = {};
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

    /* Loopback (RFC 1122): 127.0.0.0/8 */
    if (netAddr[0] == 127) {
        return true;
    }

    /* RFC 1918: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 */
    if (netAddr[0] == 10) {
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

#ifdef DATAKIT_TEST
#include "ctest.h"

/* Deterministic checks of the kernel-file parsers on synthetic input;
 * nothing here reads /proc or needs privileges */
int OSRegulateTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("network classes of crafted addresses; the system readers") {
        static const struct {
            const char *addr;
            bool isPrivate;
            bool isAll;
        } v4[] = {{"10.1.2.3", true, false},    {"172.16.9.9", true, false},
                  {"172.32.0.1", false, false}, {"192.168.1.1", true, false},
                  {"127.0.0.1", true, false},   {"8.8.8.8", false, false},
                  {"0.0.0.0", false, true}};
        for (size_t i = 0; i < sizeof(v4) / sizeof(*v4); i++) {
            struct sockaddr_in sa = {.sin_family = AF_INET};
            inet_pton(AF_INET, v4[i].addr, &sa.sin_addr);
            struct addrinfo ai = {.ai_family = AF_INET,
                                  .ai_addr = (struct sockaddr *)&sa};
            if (OSRegulateNetworkIsPrivateIPv4(&sa) != v4[i].isPrivate ||
                OSRegulateNetworkIsPrivate(&ai) != v4[i].isPrivate ||
                OSRegulateNetworkIsAllIPv4(&sa) != v4[i].isAll ||
                OSRegulateNetworkIsAll(&ai) != v4[i].isAll) {
                ERR("IPv4 %s classified wrongly", v4[i].addr);
            }
        }
        static const struct {
            const char *addr;
            bool isPrivate;
            bool isAll;
        } v6[] = {{"fc00::1", true, false},
                  {"fd12::5", true, false},
                  {"::1", true, false},
                  {"2001:db8::1", false, false},
                  {"::", false, true}};
        for (size_t i = 0; i < sizeof(v6) / sizeof(*v6); i++) {
            struct sockaddr_in6 sa6 = {.sin6_family = AF_INET6};
            inet_pton(AF_INET6, v6[i].addr, &sa6.sin6_addr);
            struct addrinfo ai = {.ai_family = AF_INET6,
                                  .ai_addr = (struct sockaddr *)&sa6};
            if (OSRegulateNetworkIsPrivateIPv6(&sa6) != v6[i].isPrivate ||
                OSRegulateNetworkIsPrivate(&ai) != v6[i].isPrivate ||
                OSRegulateNetworkIsAllIPv6(&sa6) != v6[i].isAll ||
                OSRegulateNetworkIsAll(&ai) != v6[i].isAll) {
                ERR("IPv6 %s classified wrongly", v6[i].addr);
            }
        }

        int64_t selfRSS = 0;
        int64_t childRSS = 0;
        float sysCPU = 0;
        float userCPU = 0;
        float childSys = 0;
        float childUser = 0;
        size_t fdLimit = 0;
        char *status = NULL;
        if (OSRegulateCPUCountGet() < 1 || OSRegulateTotalMemoryGet() == 0 ||
            !OSRegulateResourceUsageGet(&selfRSS, &childRSS, &sysCPU, &userCPU,
                                        &childSys, &childUser) ||
            selfRSS <= 0 || userCPU < 0 ||
            !OSRegulateAdjustOpenFilesLimit(16, &fdLimit, &status) ||
            fdLimit < 16 || OSRegulateTFOMode() < 0 ||
            !OSRegulateTcpBacklogMeetsLimit(1)) {
            ERR("a system reader failed (rss %" PRId64 ", fd limit %zu: %s)",
                selfRSS, fdLimit, status ? status : "");
        }
    }

    TEST("/proc/<pid>/stat RSS field, comm with spaces and parentheses") {
        /* Fields 3..23 are filler ("S", then 1 to 20); field 24 (RSS) is
         * the value under test.  Each comm holds what a naive space count
         * trips on. */
        static const struct {
            const char *comm;
            const char *rss;
            uint64_t max;
            bool ok;
            uint64_t want;
        } cases[] = {
            {"(cat)", "4321", UINT64_MAX, true, 4321},
            {"(my prog)", "777", UINT64_MAX, true, 777},
            {"(a b c d e f g h)", "5", UINT64_MAX, true, 5},
            {"(evil) 1 2 3)", "99", UINT64_MAX, true, 99},
            {"((nested) (x))", "123456789", UINT64_MAX, true, 123456789},
            {"())", "0", UINT64_MAX, true, 0},
            {"( )", "18446744073709551615", UINT64_MAX, true, UINT64_MAX},
            {"(big)", "18446744073709551616", UINT64_MAX, false, 0},
            {"(cap)", "1001", 1000, false, 0},
            {"(neg)", "-1", UINT64_MAX, false, 0},
            {"(junk)", "12x", UINT64_MAX, false, 0},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            char line[256];
            snprintf(line, sizeof(line),
                     "4242 %s S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 "
                     "19 20 %s 22 23\n",
                     cases[i].comm, cases[i].rss);

            uint64_t pages = 31337;
            const bool ok = osrProcStatRssPages_(line, cases[i].max, &pages);
            if (ok != cases[i].ok || (ok && pages != cases[i].want) ||
                (!ok && pages != 31337)) {
                ERR("stat line '%s': ok=%d pages=%" PRIu64 ", want ok=%d "
                    "pages=%" PRIu64,
                    line, ok, pages, cases[i].ok, cases[i].want);
            }
        }

        /* Lines that end too early or have no comm at all */
        static const char *const malformed[] = {
            "",
            "4242 cat S 1 2 3",
            "4242 (cat) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20",
            "4242 (cat)  S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 "
            "21 22",
            "4242 (cat)S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 "
            "21 22",
        };
        for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); i++) {
            uint64_t pages = 31337;
            if (osrProcStatRssPages_(malformed[i], UINT64_MAX, &pages) ||
                pages != 31337) {
                ERR("malformed stat line '%s' parsed", malformed[i]);
            }
        }
    }

    TEST("somaxconn backlog check: unreadable and unparsable both fail") {
        static const struct {
            const char *contents; /* NULL: the file could not be read */
            int requested;
            bool fits;
        } cases[] = {
            {NULL, 128, false},
            {"", 128, false},
            {"\n", 128, false},
            {"junk\n", 128, false},
            {"-1\n", 128, false},
            {"4096x\n", 128, false},
            {"99999999999999999999\n", 128, false},
            {"2147483648\n", 128, false}, /* above INT_MAX */
            {"4096\n", 4096, true},
            {"4096\n", 4097, false},
            {"4096\n", 511, true},
            {"128\n", 511, false},
            {"0\n", 511, true},  /* no limit configured */
            {"128\n", 0, true},  /* nothing requested */
            {"128\n", -5, true}, /* nothing requested */
            {"2147483647\n", INT_MAX, true},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            if (osrBacklogFits_(cases[i].contents, cases[i].requested) !=
                cases[i].fits) {
                ERR("somaxconn '%s' for %d: want fits=%d",
                    cases[i].contents ? cases[i].contents : "(unreadable)",
                    cases[i].requested, cases[i].fits);
            }
        }
    }

    TEST("this process has a resident set") {
        if (OSRegulateRSSGet() == 0) {
            ERRR("OSRegulateRSSGet returned 0 for a running process");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
