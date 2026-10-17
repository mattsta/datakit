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

/* Parent-death watch fd.  On kqueue platforms (macOS/BSD) returns a
 * kqueue fd armed with a one-shot EVFILT_PROC/NOTE_EXIT filter on
 * `parent`; block in kevent() on it to receive an event the instant
 * the parent process exits.  Returns -1 on Linux (PR_SET_PDEATHSIG
 * covers that case) or on error (caller should fall back to polling). */
int OSRegulateParentDeathKqueueFd(pid_t parent);

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
