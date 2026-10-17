#pragma once

#include "datakit.h"
#include <arpa/inet.h> /* sockaddr[6]_in, inet_pton, ... */

/* struct addrinfo */
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef enum OSRegulateDaemonizeStatus {
    OS_REGULATE_DAEMONIZE_PARENT,
    OS_REGULATE_DAEMONIZE_CHILD,
    OS_REGULATE_DAEMONIZE_ERROR
} OSRegulateDaemonizeStatus;

/* Process */
OSRegulateDaemonizeStatus OSRegulateDaemonize(const char **errMsg);
bool OSRegulateWritePidToFile(const char *path);
void OSRegulateDaemonizeThenExitNoWait(void);
void OSRegulateExitClean(void);

/* Readiness channel: a one-shot cross-process "ready" event.  The
 * child sends one byte when ready; the parent waits on the read end and
 * also sees end-of-file if every writer closes without signalling (the
 * child died), which an eventfd could not report. */
typedef enum OSRegulateReadyAwait {
    OS_REGULATE_READY_AWAIT_READY,       /* the writer signalled */
    OS_REGULATE_READY_AWAIT_WRITER_GONE, /* no writer left, or unwaitable */
    OS_REGULATE_READY_AWAIT_EXPIRED,     /* budget elapsed, writer alive */
} OSRegulateReadyAwait;

/* Create the pair.  Both ends are close-on-exec; the read end is meant
 * for the parent and the write end for the child (each closes the
 * other's end after the fork).  The caller owns both descriptors. */
bool OSRegulateReadyPipeNew(int *outRead, int *outWrite);

/* Signal readiness once and close the write end (idempotent on a
 * descriptor already signalled; a negative descriptor is a no-op). */
void OSRegulateReadyPipeSignal(int *inoutWriteFd);

/* Wait until the writer signals, every writer is gone, or `budgetMs`
 * elapses.  Signals interrupting the wait do not extend the budget.
 * WRITER_GONE is also returned when readiness can no longer be
 * observed: a negative readFd, or a poll()/read() failure other than
 * EINTR. */
OSRegulateReadyAwait OSRegulateReadyPipeAwaitMs(int readFd, uint64_t budgetMs);

/* Daemonize, then hold the foreground parent until the child signals
 * readiness: the parent exits 0 on READY and nonzero if the child exits
 * first or readyBudgetMs elapses.  Both exits use _exit(), so the
 * parent runs no atexit handlers.  The child gets
 * OS_REGULATE_DAEMONIZE_CHILD and calls OSRegulateDaemonizeSignalReady()
 * once it is ready to serve. */
OSRegulateDaemonizeStatus OSRegulateDaemonizeUntilReady(uint64_t readyBudgetMs,
                                                        const char **errMsg);
void OSRegulateDaemonizeSignalReady(void);

bool OSRegulateRequestSignalChildWhenParentExits(int sig);
bool OSRegulateParentStillExisits(void);

/* Waiting for a process to exit.  waitpid() has no timeout, so these
 * wait on a pollable exit notification instead: kqueue
 * EVFILT_PROC/NOTE_EXIT on macOS and the BSDs, a pidfd on Linux.  The
 * watch fd works for any observable pid; ReapNow and AwaitExitMs
 * require a child of the caller.  Like any pid-based API they assume
 * nothing else reaps the child: if another thread reaps it and the pid
 * is reused, they act on the new process. */

/* Returns a descriptor that becomes readable when `pid` exits, or -1
 * when the platform has no exit notification (errno ENOSYS) or arming
 * failed (which includes `pid` already having exited).  -1 never means
 * "still running".  errno is preserved on failure; for an unreaped
 * child, ESRCH means its exit is already under way.  The caller closes
 * the fd. */
int OSRegulateProcessExitWatchFd(pid_t pid);

/* Reap `pid` if it has already exited; true when reaped, or when
 * waitpid() reports ECHILD (already reaped elsewhere, or not a child).
 * In the ECHILD case the exit status is unknown and *outStatus is set
 * to 0.  Never blocks. */
bool OSRegulateProcessReapNow(pid_t pid, int *outStatus);

/* Wait up to `budgetMs` for child `pid` to exit, then reap it.  Returns
 * true only if it was reaped within the budget (see ReapNow for the
 * ECHILD case).  Never signals the child, and signals interrupting the
 * wait do not extend the budget.  Once an exit has been reported (the
 * watch fd became readable, or arming failed with ESRCH) the reap
 * blocks, because the notification can precede the point where a
 * WNOHANG reap succeeds; otherwise the final reap is non-blocking.
 * Without an exit notification on the platform it does not wait: it
 * only makes non-blocking reap attempts. */
bool OSRegulateProcessAwaitExitMs(pid_t pid, uint64_t budgetMs, int *outStatus);

/* Wake descriptor: an async-signal-safe wakeup for a loop that reacts
 * to events raised in signal handlers or other threads (a condition
 * variable cannot be signalled from a handler).  An eventfd on Linux,
 * a non-blocking close-on-exec pipe elsewhere.  Wakes coalesce, so the
 * consumer must re-check its state after each wake rather than count
 * wakes. */

/* Create the pair.  On success *outRead is the descriptor to poll and
 * *outWrite the one to signal (equal on eventfd platforms).  Both are
 * non-blocking and close-on-exec; the caller owns and closes both
 * (compare the two before closing — one descriptor on eventfd). */
bool OSRegulateWakeFdNew(int *outRead, int *outWrite);

/* Signal the wake from any context, signal handlers included.  Never
 * blocks; a full pipe (a wake already pending) is success.  Preserves
 * errno. */
void OSRegulateWakeSignal(int writeFd);

/* Consume every pending wake.  Never blocks. */
void OSRegulateWakeDrain(int readFd);

/* Wait until the wake is signalled or `budgetMs` elapses; drains on a
 * wake.  Returns true when woken, false on expiry or poll() failure.
 * Signals interrupting the wait do not extend the budget. */
bool OSRegulateWakeAwaitMs(int readFd, uint64_t budgetMs);

/* CPUs */
size_t OSRegulateCPUCountGet(void);

/* Networking */
bool OSRegulateTcpBacklogMeetsLimit(int tcpBacklogListenLength);
int OSRegulateTFOMode();

/* Files */
bool OSRegulateAdjustOpenFilesLimit(const size_t requestedFdCount,
                                    size_t *limitActuallySet, char **statusMsg);

/* Usage */
bool OSRegulateResourceUsageGet(long *selfMaxRSS, long *childMaxRSS,
                                float *selfSystemCPU, float *selfUserCPU,
                                float *childSystemCPU, float *childUserCPU);

/* Memory */
bool OSRegulateLinuxOvercommitEnabled(void);
bool OSRegulateLinuxTransparentHugePagesEnabled(void);
size_t OSRegulateLinuxSmapBytesByFieldForPid(const char *field, int64_t pid);
size_t OSRegulateLinuxTransparentHugePagesGetAnonHugePagesSize(int64_t pid);
size_t OSRegualteLinuxSmapPrivateDirtyGet(int64_t pid);
bool OSRegulateLinuxMemorySettingsAreOkay(void);
size_t OSRegulateTotalMemoryGet(void);
size_t OSRegulateRSSGet(void);

/* Network */
bool OSRegulateNetworkIsPrivate(const struct addrinfo *ai);
bool OSRegulateNetworkIsPrivateIPv4(const struct sockaddr_in *sa);
bool OSRegulateNetworkIsPrivateIPv6(const struct sockaddr_in6 *sa6);

bool OSRegulateNetworkIsAll(const struct addrinfo *ai);
bool OSRegulateNetworkIsAllIPv4(const struct sockaddr_in *sa);
bool OSRegulateNetworkIsAllIPv6(const struct sockaddr_in6 *sa6);
